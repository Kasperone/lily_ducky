// =============================================================================
// app_main.c - ESP-IDF SoftAP boot for the T-Dongle-C5-idf environment
// =============================================================================
// Provenance: this file and every other source in src/idf_softap/ come from the
// hybrid SoftAP firmware that passed on real hardware and closed issue #9
// (win11 + Hama RTL8188EUS associated, HTTP 200 through every layer, PCAP
// download, LCD dashboard). It was developed outside the repo as a flat IDF
// tree; here it is one component listed by src/CMakeLists.txt, and it is the
// ONLY thing the T-Dongle-C5-idf env builds. The Arduino environments ignore
// this directory entirely (build_src_filter in platformio.ini).
//
// SoftAP bring-up below is deliberately raw esp_netif + esp_wifi: no
// arduino-esp32 WiFi wrapper and no prebuilt Arduino WiFi/PHY blobs, which is
// exactly the difference that fixed the AP->STA downlink wedge. Do not
// "simplify" it toward the Arduino WiFi API.
// =============================================================================
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "board_spi.h"
#include "c2_server.h"
#include "duck_display.h"
#include "interpreter.h"
#include "recon.h"
// Credentials come from the project's gitignored firmware/src/config_secret.h
// (see softap_secret.h) rather than a second copy in this directory.
#include "softap_secret.h"
#include "storage.h"
static const char *T = "idf_softap";

// ── Interpreter hooks (layer 3b) ────────────────────────────────────────────
// GET /api/status "state": the two enums have the same ordering by
// construction, but the mapping is written out so that reordering either one
// becomes a compile error / visible mismatch instead of a silently wrong
// status string.
static c2_interp_state_t hook_interp_state(void)
{
  switch (duck_interp_state()) {
    case DUCK_INTERP_IDLE:     return C2_INTERP_IDLE;
    case DUCK_INTERP_RUNNING:  return C2_INTERP_RUNNING;
    case DUCK_INTERP_COMPLETE: return C2_INTERP_COMPLETE;
    case DUCK_INTERP_ERROR:    return C2_INTERP_ERROR;
    case DUCK_INTERP_STOPPED:  return C2_INTERP_STOPPED;
  }
  return C2_INTERP_IDLE;
}

// POST /api/run/<name>: read /sdcard/payloads/<name> and hand it to the
// interpreter. Returns false when the file is missing/unreadable, which is the
// hook contract c2_server.c turns into its 404 "Payload not found". Execution
// is asynchronous (dedicated interpreter task), so this never blocks httpd.
static bool hook_run_payload(const char *name)
{
  char *buf = NULL;
  size_t len = 0;
  if (!storage_read_payload(name, &buf, &len)) {
    ESP_LOGW(T, "run: payload '%s' not found", name);
    return false;
  }
  ESP_LOGI(T, "run: payload '%s' (%u bytes)", name, (unsigned)len);
  // duck_interp_start() copies the script before returning, so the buffer can
  // be released here.
  bool ok = duck_interp_start(buf, len);
  free(buf);
  if (!ok) ESP_LOGE(T, "run: interpreter refused '%s'", name);
  return ok;
}

// POST /api/stop.
static void hook_stop_payload(void)
{
  duck_interp_stop();
}

// ── Recon hooks (layer 3c) ─────────────────────────────────────────────────
// Thin adapters: recon.c owns the radio, the ring buffer and the SD writes; the
// C2 layer only ever sees these. Every recon* pointer in c2_hooks_t is non-NULL
// below, so /api/recon/* answers with real data instead of the layer-2
// "unavailable" shapes.
static bool hook_recon_start_capture(char *outName, size_t outNameLen)
{
  return recon_start_capture(outName, outNameLen);
}

static uint32_t hook_recon_packet_count(void) { return recon_packet_count(); }
static uint32_t hook_recon_dropped_count(void) { return recon_dropped_count(); }

// reconApRecord: formats one row into the caller's buffers. Must return false
// when idx is out of range, which is how c2_server.c stops iterating.
static bool hook_recon_ap_record(uint32_t idx, char *bssid, size_t bssidLen,
                                 char *ssid, size_t ssidLen, bool *hidden,
                                 uint8_t *channel, bool *band5, int8_t *rssi,
                                 uint8_t *authmode, bool *pmf,
                                 const char **pmfStatus)
{
  const recon_ap_record_t *r = recon_ap_record(idx);
  if (!r) return false;

  if (bssid && bssidLen) {
    snprintf(bssid, bssidLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             r->bssid[0], r->bssid[1], r->bssid[2],
             r->bssid[3], r->bssid[4], r->bssid[5]);
  }
  if (ssid && ssidLen) {
    snprintf(ssid, ssidLen, "%s", r->ssid);
  }
  if (hidden) *hidden = r->hidden;
  if (channel) *channel = r->channel;
  if (band5) *band5 = r->band5;
  if (rssi) *rssi = r->rssi;
  if (authmode) *authmode = r->authmode;
  if (pmf) *pmf = r->pmf;
  // Always one of disabled/capable/required/unknown (see recon_pmf_status_str).
  if (pmfStatus) *pmfStatus = recon_pmf_status_str(r->pmfStatus);
  return true;
}

static uint32_t hook_recon_sta_count(void) { return recon_sta_count(); }

static bool hook_recon_sta_record(uint32_t idx, char *mac, size_t macLen,
                                  int8_t *rssi, uint32_t *lastSeenMs)
{
  const recon_sta_record_t *s = recon_sta_record(idx);
  if (!s) return false;
  if (mac && macLen) {
    snprintf(mac, macLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
  }
  if (rssi) *rssi = s->rssi;
  if (lastSeenMs) *lastSeenMs = s->lastSeenMs;
  return true;
}

// Layer-3c hook table: interpreter (3b), payload storage (3a) and the recon
// radio (3c) are all real. Display is not a C2 hook - it is a local init plus
// periodic update driven from app_main's alive loop.
static const c2_hooks_t hooks = {
  .getInterpState = hook_interp_state,
  .listPayloads = storage_list_payloads,
  .payloadCount = storage_payload_count,
  .payloadName = storage_payload_name,
  .payloadSize = storage_payload_size,
  .readPayload = storage_read_payload,
  .writePayload = storage_write_payload,
  .runPayload = hook_run_payload,
  .stopPayload = hook_stop_payload,

  .reconStartCapture = hook_recon_start_capture,
  .reconStopCapture = recon_stop_capture,
  .reconCapturing = recon_capturing,
  .reconCurrentFile = recon_current_file,
  .reconPacketCount = hook_recon_packet_count,
  .reconDroppedCount = hook_recon_dropped_count,
  // GET /api/recon/pcap/<name>: binary-safe /sdcard/recon/<name> read.
  .reconReadPcap = storage_read_recon,

  .reconBusy = recon_busy,
  .reconStartScan = recon_start_scan,
  .reconScanning = recon_scanning,
  .reconLastScanMs = recon_last_scan_ms,
  .reconApCount = recon_ap_count,
  .reconApRecord = hook_recon_ap_record,

  .reconStartEnum = recon_start_enum,
  .reconEnumRunning = recon_enum_running,
  .reconStaCount = hook_recon_sta_count,
  .reconStaRecord = hook_recon_sta_record,
};

// ── Display model (built from the live subsystems, ~1 Hz) ───────────────────// The dashboard is fed a snapshot rather than reading subsystems itself, so it
// never has to know about the interpreter, httpd or the wifi driver.
static void build_display_model(duck_display_model_t *m)
{
  memset(m, 0, sizeof(*m));

  m->interpState = (int)duck_interp_state();

  wifi_sta_list_t l;
  if (esp_wifi_ap_get_sta_list(&l) == ESP_OK) m->clients = l.num;

  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_ip_info_t ip;
  if (ap && esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
    snprintf(m->ip, sizeof(m->ip), IPSTR, IP2STR(&ip.ip));
  } else {
    snprintf(m->ip, sizeof(m->ip), "192.168.4.1");
  }

  const char *tok = c2_server_auth_token();
  snprintf(m->token, sizeof(m->token), "%s", tok ? tok : "");

  m->reconCapturing = recon_capturing();
  m->reconPackets = recon_packet_count();

  duck_interp_progress(&m->pc, &m->lineCount, m->currentLine,
                       sizeof(m->currentLine));
}

static void ev(void *a, esp_event_base_t b, int32_t id, void *d){
  if(id==WIFI_EVENT_AP_STACONNECTED){
    wifi_event_ap_staconnected_t*e=d;
    ESP_LOGI(T,"STA join " MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
  } else if(id==WIFI_EVENT_AP_STADISCONNECTED){
    wifi_event_ap_stadisconnected_t*e=d;
    ESP_LOGI(T,"STA leave " MACSTR " reason=%d", MAC2STR(e->mac), e->reason);
  }
}
void app_main(void){
  esp_err_t r=nvs_flash_init();
  if(r==ESP_ERR_NVS_NO_FREE_PAGES||r==ESP_ERR_NVS_NEW_VERSION_FOUND){nvs_flash_erase();nvs_flash_init();}
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();
  // Recon (layer 3c) needs a station interface to exist because esp_wifi only
  // accepts esp_wifi_scan_start() when one is present. Creating the default STA
  // netif does NOT connect anything: it stays at 0.0.0.0, is never given
  // credentials, and only ever comes up for the brief APSTA window a scan
  // opens. It has to be created before esp_wifi_start() (IDF requirement), and
  // it deliberately sits beside the AP netif rather than inside recon.c so the
  // SoftAP's own bring-up sequence below is untouched.
  esp_netif_create_default_wifi_sta();

  // Shared SPI2 (SD + LCD + APA102) before anything can want a device on it.
  // Idempotent: storage_init() and duck_display_init() both call it too, so a
  // failure here is only logged - each of those retries and reports its own.
  if(board_spi_bus_init()!=ESP_OK){
    ESP_LOGE(T,"Shared SPI bus unavailable, SD and LCD disabled");
  }

  wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&c));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,ev,NULL,NULL));
  wifi_config_t w={0};
  strcpy((char*)w.ap.ssid,CFG_WIFI_SSID);
  w.ap.ssid_len=strlen(CFG_WIFI_SSID);
  strcpy((char*)w.ap.password,CFG_WIFI_PASS);
  w.ap.channel=6;
  w.ap.max_connection=4;
  w.ap.authmode=WIFI_AUTH_WPA2_PSK;
  w.ap.pmf_cfg.required=false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&w));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(T,"SOFTAP_UP ssid=%s ch=6 ip=192.168.4.1 (stock IDF softAP+httpd)",CFG_WIFI_SSID);
  httpd_handle_t s=NULL;
  httpd_config_t hc=HTTPD_DEFAULT_CONFIG();
  // The C2 route table needs more slots than the default 8, and the wildcard
  // "/api/payload/*"-style URIs require the wildcard matcher (the default NULL
  // matcher is a plain strncmp).
  hc.max_uri_handlers=C2_URI_HANDLER_COUNT;
  hc.uri_match_fn=httpd_uri_match_wildcard;
  // Interpreter (layer 3b): one task that ticks the DuckyScript VM. Started
  // before the routes are registered so GET /api/status never reports
  // "no_interp" once the server is up. A failure here is not fatal - run/stop
  // then answer 404/200 with the interpreter left idle, as in layer 3a.
  if(duck_interp_init()!=ESP_OK){
    ESP_LOGE(T,"Interpreter task unavailable, /api/run disabled");
  }
  if(httpd_start(&s,&hc)==ESP_OK){
    // LilyDucky C2 surface. Payload storage (3a), the interpreter (3b) and the
    // recon radio (3c) are all wired; see the hook table above.
    if(c2_server_register(s,&hooks)!=ESP_OK){
      ESP_LOGE(T,"C2 route registration failed");
    }
    ESP_LOGI(T,"HTTPD_UP :80");
  }
  // SD card (layer 3a). A missing card is not fatal: log and keep serving. The
  // shared SPI bus is NOT torn down on failure - the LCD still needs it.
  if(storage_init()!=ESP_OK){
    ESP_LOGW(T,"SD card unavailable, payload/recon-capture endpoints disabled");
  }
  // Display (layer 3c): after storage_init() so the shared SPI bus already
  // exists and the LCD just adds its second spi_device. Recon is initialised
  // after WiFi is up (its tick task assumes the driver is started) and after
  // storage (captures need /sdcard).
  if(duck_display_init()!=ESP_OK){
    ESP_LOGW(T,"Status display unavailable, continuing headless");
  }
  if(recon_init()!=ESP_OK){
    ESP_LOGW(T,"Recon unavailable, /api/recon/* will report unavailable");
  }
  ESP_LOGI(T,"boot complete (display=%s, recon=%s, sd=%s)",
           duck_display_ready()?"up":"off",
           recon_busy()?"busy":"idle",
           storage_mounted()?"mounted":"absent");

  // Alive loop. 1 Hz: refreshed status on the panel, and the heartbeat log that
  // layer 3b already emitted. 10 s would be too slow to see a payload progress
  // on the LCD; 1 s of logging is still cheap enough to leave the console
  // readable.
  duck_display_model_t model;
  int tick = 0;
  while(1){
    vTaskDelay(pdMS_TO_TICKS(1000));
    build_display_model(&model);
    duck_display_update(&model);
    if(++tick >= 10){
      tick = 0;
      wifi_sta_list_t l;
      esp_wifi_ap_get_sta_list(&l);
      ESP_LOGI(T,"alive sta=%d heap=%u%s",l.num,(unsigned)esp_get_free_heap_size(),
               recon_capturing()?" [CAP]":"");
    }
  }
}
