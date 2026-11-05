// =============================================================================
// recon.c - 802.11 recon on pure ESP-IDF (see recon.h for the contract)
// =============================================================================
#include "recon.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "board_pins.h"
#include "board_spi.h"
#include "pcap_writer.h"
#include "recon_config.h"
#include "storage.h"

static const char *TAG = "recon";

// Directory under the storage mount point. Matches C2_SD_RECON_DIR in
// c2_server.h ("/recon"), so /recon on the card is /sdcard/recon in the VFS.
#define RECON_DIR STORAGE_MOUNT_POINT "/recon"
// Longest basename we ever generate ("recon_<10 digits>.pcap").
#define RECON_MAX_BASENAME 40

// ── Ring buffer ─────────────────────────────────────────────────────────────
// Single producer (the Wi-Fi driver task's promiscuous callback) / single
// consumer (the recon tick task). The C5 is single-core (CONFIG_FREERTOS_UNICORE
// is set in sdkconfig), so the only hazard is preemption between the two tasks;
// a release-store on head and an acquire-load on both indices is enough, and
// costs nothing on RISC-V.
typedef struct {
    uint8_t  data[CFG_RECON_SNAPLEN];
    uint16_t len;          // captured length (<= CFG_RECON_SNAPLEN)
    uint16_t origLen;      // true on-air length (rx_ctrl.sig_len), may exceed len
    uint32_t timestampUs;
    int8_t   rssi;
    uint8_t  channel;
    bool     isEapol;      // CAPTURE mode only: true for WIFI_PKT_DATA (EAPOL)
} recon_frame_t;

static recon_frame_t s_ring[CFG_RECON_RING_SLOTS];
static uint16_t s_head;    // producer index (release-stored)
static uint16_t s_tail;    // consumer index (only the tick task writes it)
static volatile uint32_t s_dropped;
static volatile uint32_t s_packetCount;

// Which consumer the tick drains, and which classifier the callback applies.
// Only one of these ever owns the radio at a time (see radio_busy()).
typedef enum {
    RING_NONE = 0,
    RING_CAPTURE,
    RING_STA_ENUM,
} ring_mode_t;
static volatile ring_mode_t s_ringMode = RING_NONE;

static char s_currentFile[RECON_MAX_BASENAME] = "";
static volatile bool s_capturing;
// Set for the duration of recon_stop_capture(). The tick task skips its own
// drain while this is up, so the shutdown path is the only consumer of whatever
// is still queued - otherwise the two would race for the tail index and some of
// the capture's last frames would be attributed to the wrong drain.
static volatile bool s_captureStopping;

// ── AP table (scan results) ─────────────────────────────────────────────────
static recon_ap_record_t s_aps[CFG_RECON_SCAN_MAX_APS];
static volatile uint32_t s_apCount;
static volatile bool s_scanning;
static volatile uint32_t s_lastScanMs;
static volatile bool s_scanDoneFlag;   // set by the WIFI_EVENT_SCAN_DONE handler
// Set by recon_start_scan() and consumed by the tick task, which is the only
// place that touches the WiFi control plane (mode switches, channel changes,
// scan_start). Keeping those off the httpd task and off the event handler is
// the issue #9 mitigation: the reference firmware's hang came from a
// heavyweight control-plane call landing immediately after the async scan's own
// cleanup, and deferring by at least one tick is what fixed it.
static volatile bool s_scanPending;

// ── Station table (enum results) ────────────────────────────────────────────
static recon_sta_record_t s_stas[CFG_RECON_MAX_STAS];
static volatile uint32_t s_staCount;
static volatile bool s_enumRunning;
static volatile bool s_enumPending;   // start requested, channel hop not done yet
static uint8_t  s_enumTargetBssid[6];
static uint32_t s_enumApIdx;
static uint32_t s_enumStartMs;
static uint8_t  s_enumAttempt;
static uint8_t  s_enumChannel;

static bool s_ready;
static TaskHandle_t s_task;

// Serialises the public start/stop entry points against each other. httpd is
// single-task today, but these hooks are a public surface: a second httpd task
// (or a future console command) calling recon_start_scan() while another is in
// recon_start_capture() must not be able to interleave the two radio
// transitions. The recon tick task never takes this lock, so it cannot deadlock
// against it.
static SemaphoreHandle_t s_ctlMutex;

// Deferred AP-only restore. Declared here because radio_busy() treats a pending
// restore as "radio still borrowed". Defined/used in the SoftAP mode section.
static volatile bool s_restorePending;
static volatile uint32_t s_restoreAtMs;
// True when this module changed the WiFi mode (AP -> APSTA) and therefore owes
// the SoftAP a restore. Without it, a build that was already APSTA would be
// silently forced back to AP-only, which is main.c's decision, not ours.
static volatile bool s_radioBorrowed;
// True when this module moved the radio off the AP's channel. Tracked
// separately from s_radioBorrowed because the two are independent: an enum can
// hop the channel without a mode switch when the driver is already APSTA, and
// that hop still has to be undone - otherwise the SoftAP ends up serving on
// whatever channel the enumeration last visited (the issue #9 failure mode).
static volatile bool s_channelBorrowed;
// When the in-flight scan started. Used by the tick task as a watchdog: if
// WIFI_EVENT_SCAN_DONE never arrives, the radio would otherwise stay marked
// busy forever and every later capture/scan/enum would be refused.
static volatile uint32_t s_scanStartMs;

// ── Helpers ─────────────────────────────────────────────────────────────────
static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// True while any mode owns the single radio. Every start* checks this.
// s_restorePending counts as busy: the AP mode/channel has not been put back
// yet, so a new capture must not start on the borrowed channel.
static bool radio_busy(void)
{
    return s_capturing || s_scanning || s_scanPending || s_enumRunning ||
           s_enumPending || s_scanDoneFlag || s_restorePending;
}

bool recon_busy(void) { return radio_busy(); }

static bool ensure_recon_dir(void)
{
    if (!storage_mounted()) return false;
    // mkdir on FATFS is SD traffic on the shared bus, so it is serialised with
    // LCD paints like any other access.
    board_spi_lock();
    int rc = mkdir(RECON_DIR, 0775);
    int err = errno;
    board_spi_unlock();
    if (rc != 0 && err != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", RECON_DIR, err);
        return false;
    }
    return true;
}

// ── Frame classifiers (cheap, run in the Wi-Fi driver task) ─────────────────
// Data frame -> EAPOL: standard 802.11 header (24 B, +2 B QoS Control when the
// subtype's QoS bit is set) then an 802.2 LLC/SNAP header (AA AA 03 00 00 00
// <ethertype>) with EtherType 0x888E. Plain protocol decoding, not
// chip-specific. Not handled: WDS (+Addr4) and +HTC/Order frames, neither of
// which this lab topology produces - a documented gap, same as lily.
static bool is_eapol_data_frame(const uint8_t *f, uint16_t len)
{
    if (len < 24) return false;
    uint8_t fc0 = f[0];
    uint8_t type = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0xF;
    if (type != 2) return false;  // not Data

    int hdrLen = 24;
    if (subtype & 0x08) hdrLen += 2;  // QoS Data carries a QoS Control field
    if (len < (uint16_t)(hdrLen + 8)) return false;

    const uint8_t *llc = f + hdrLen;
    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
        llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00) {
        uint16_t etherType = ((uint16_t)llc[6] << 8) | llc[7];
        return etherType == 0x888E;  // EAPOL
    }
    return false;
}

// Resolves the non-BSSID address of a Data frame via its ToDS/FromDS bits, iff
// the frame's BSSID matches `target`. Returns false for WDS (+Addr4, both bits
// set) and IBSS/no-DS frames (neither bit set) - same documented gap as lily.
static bool resolve_station_mac(const uint8_t *f, uint16_t len,
                                const uint8_t target[6], uint8_t staMacOut[6])
{
    if (len < 24) return false;
    uint8_t fc1 = f[1];
    bool toDS = fc1 & 0x1;
    bool fromDS = (fc1 >> 1) & 0x1;
    if (toDS == fromDS) return false;  // WDS or IBSS/no-DS - not our topology

    const uint8_t *addr1 = f + 4;
    const uint8_t *addr2 = f + 10;
    const uint8_t *bssid = fromDS ? addr2 : addr1;
    const uint8_t *station = fromDS ? addr1 : addr2;
    if (memcmp(bssid, target, 6) != 0) return false;
    memcpy(staMacOut, station, 6);
    return true;
}

// Runs in the Wi-Fi driver task. Kept to cheap branches plus one bounded
// memcpy: no logging, no SD, no malloc - all of that is deferred to tick().
static void promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    ring_mode_t mode = s_ringMode;
    if (mode == RING_NONE) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *frame = pkt->payload;
    uint16_t origLen = (uint16_t)pkt->rx_ctrl.sig_len;
    // A zero/garbage length would make the memcpy below read past the buffer.
    if (origLen < 10) return;

    bool accept = false;
    switch (mode) {
        case RING_CAPTURE:
            // Data frames: only EAPOL, so ordinary client traffic (the Win11
            // VM's browsing, dashboard HTTP, ...) never enters the ring.
            // Management frames (beacon/probe/auth/assoc/deauth) are all kept.
            accept = (type == WIFI_PKT_MGMT) || is_eapol_data_frame(frame, origLen);
            break;
        case RING_STA_ENUM: {
            if (type != WIFI_PKT_DATA) break;
            uint8_t staMac[6];
            accept = resolve_station_mac(frame, origLen, s_enumTargetBssid, staMac);
            break;
        }
        default:
            break;
    }
    if (!accept) return;

    uint16_t head = __atomic_load_n(&s_head, __ATOMIC_RELAXED);
    uint16_t next = (uint16_t)((head + 1) % CFG_RECON_RING_SLOTS);
    if (next == __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE)) {
        // Ring full: drop rather than block. The Wi-Fi task must never wait on
        // our consumer, so a burst simply costs frames and bumps this counter.
        __atomic_store_n(&s_dropped, s_dropped + 1, __ATOMIC_RELAXED);
        return;
    }

    recon_frame_t *slot = &s_ring[head];
    uint16_t copyLen = origLen < CFG_RECON_SNAPLEN ? origLen : CFG_RECON_SNAPLEN;
    memcpy(slot->data, frame, copyLen);
    slot->len = copyLen;
    slot->origLen = origLen;
    slot->timestampUs = pkt->rx_ctrl.timestamp;
    slot->rssi = (int8_t)pkt->rx_ctrl.rssi;
    slot->channel = (uint8_t)pkt->rx_ctrl.channel;
    slot->isEapol = (mode == RING_CAPTURE) && (type == WIFI_PKT_DATA);

    // Release-store publishes the fully written slot to the consumer.
    __atomic_store_n(&s_head, next, __ATOMIC_RELEASE);
}

// ── Capture ─────────────────────────────────────────────────────────────────
static bool promiscuous_on_masked(uint32_t mask)
{
    wifi_promiscuous_filter_t filter = { .filter_mask = mask };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK) return false;
    err = esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);
    if (err != ESP_OK) return false;
    err = esp_wifi_set_promiscuous(true);
    return err == ESP_OK;
}

bool recon_start_capture(char *outName, size_t outNameLen)
{
    if (!s_ready || !s_ctlMutex) return false;
    if (xSemaphoreTake(s_ctlMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    if (radio_busy()) {   // one radio, no overlap
        xSemaphoreGive(s_ctlMutex);
        return false;
    }
    if (!ensure_recon_dir()) {
        ESP_LOGW(TAG, "capture refused: SD card / recon dir unavailable");
        xSemaphoreGive(s_ctlMutex);
        return false;
    }

    char basename[RECON_MAX_BASENAME];
    snprintf(basename, sizeof(basename), "recon_%lu.pcap",
             (unsigned long)now_ms());
    char fullPath[sizeof(RECON_DIR) + RECON_MAX_BASENAME + 2];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", RECON_DIR, basename);

    // PCAP open is SD traffic on the shared bus: hold the board mutex so an LCD
    // paint cannot be split into the middle of the write burst. Lock order is
    // always ctl -> bus; nothing takes them the other way.
    board_spi_lock();
    bool opened = pcap_open(fullPath);
    board_spi_unlock();
    if (!opened) {
        ESP_LOGE(TAG, "capture refused: cannot open %s", fullPath);
        xSemaphoreGive(s_ctlMutex);
        return false;
    }

    // Reset the pipeline before the callback can see any of it, under the bus
    // lock so the recon tick task cannot be half-way through a stale drain.
    board_spi_lock();
    s_ringMode = RING_NONE;
    __atomic_store_n(&s_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tail, 0, __ATOMIC_RELAXED);
    board_spi_unlock();
    __atomic_store_n(&s_dropped, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_packetCount, 0, __ATOMIC_RELAXED);
    strncpy(s_currentFile, basename, sizeof(s_currentFile) - 1);
    s_currentFile[sizeof(s_currentFile) - 1] = '\0';

    // Capture is pinned to the SoftAP's channel. Read it rather than assuming
    // BOARD_AP_CHANNEL: if anything ever moved the AP, capture must follow the
    // AP, not drag the AP to the constant (issue #9).
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);
    ESP_LOGI(TAG, "starting capture on AP channel %u (SoftAP left untouched)",
             (unsigned)primary);

    if (!promiscuous_on_masked(WIFI_PROMIS_FILTER_MASK_MGMT |
                               WIFI_PROMIS_FILTER_MASK_DATA)) {
        ESP_LOGE(TAG, "capture refused: promiscuous mode could not be enabled");
        board_spi_lock();
        pcap_close();
        board_spi_unlock();
        s_currentFile[0] = '\0';
        xSemaphoreGive(s_ctlMutex);
        return false;
    }

    // Arm the mode under the bus lock too, so the reset above and the mode set
    // are one step from the tick task's point of view.
    board_spi_lock();
    s_ringMode = RING_CAPTURE;
    s_capturing = true;
    board_spi_unlock();

    if (outName && outNameLen > 0) {
        strncpy(outName, s_currentFile, outNameLen - 1);
        outName[outNameLen - 1] = '\0';
    }
    xSemaphoreGive(s_ctlMutex);
    ESP_LOGI(TAG, "capture started -> %s", s_currentFile);
    return true;
}

void recon_stop_capture(void)
{
    if (!s_capturing) return;
    if (s_ctlMutex && xSemaphoreTake(s_ctlMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "stop_capture: control mutex busy, proceeding");
    }
    s_captureStopping = true;
    esp_wifi_set_promiscuous(false);

    // Drain whatever is left in the ring *before* closing the file, so the tail
    // of the capture is not lost - same ordering as lily's stopCapture().
    //
    // Done here rather than left to the tick task because the caller (httpd)
    // must see a closed, complete file by the time this returns. The tick task
    // is excluded from draining by s_captureStopping for the duration.
    board_spi_lock();
    while (__atomic_load_n(&s_tail, __ATOMIC_RELAXED) !=
           __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        uint16_t tail = s_tail;
        recon_frame_t *slot = &s_ring[tail];
        pcap_write_frame(slot->data, slot->len, slot->origLen,
                         slot->timestampUs, slot->rssi, slot->channel);
        __atomic_store_n(&s_packetCount, s_packetCount + 1, __ATOMIC_RELAXED);
        s_tail = (uint16_t)((tail + 1) % CFG_RECON_RING_SLOTS);
    }
    s_ringMode = RING_NONE;
    pcap_close();
    board_spi_unlock();

    s_capturing = false;
    s_captureStopping = false;
    if (s_ctlMutex) xSemaphoreGive(s_ctlMutex);
    board_led_relatch();
    ESP_LOGI(TAG, "capture stopped: %u packets, %u dropped",
             (unsigned)s_packetCount, (unsigned)s_dropped);
}

bool recon_capturing(void) { return s_capturing; }
const char *recon_current_file(void) { return s_currentFile; }
uint32_t recon_packet_count(void) { return s_packetCount; }
uint32_t recon_dropped_count(void) { return s_dropped; }

// ── SoftAP mode handling ────────────────────────────────────────────────────
// The SoftAP belongs to main.c; recon only borrows the mode briefly and always
// puts it back. All three helpers must run from the tick task, never from an
// event handler or the httpd task: the reference firmware's hang came from a
// heavyweight control-plane call landing right after the async scan's own
// cleanup, and the settled fix is to defer it - with a settling gap - by at
// least a tick.
//
// Settling gap before the AP-only restore. Mirrors lily's
// CFG_RECON_APDOWN_SETTLE_MS rationale ("a settling delay is the single most
// load-bearing part of the mitigation - do not shorten without re-validating")
// and is the whole reason the restore is deferred rather than inline.
#define RECON_RESTORE_SETTLE_MS 300

// If WIFI_EVENT_SCAN_DONE never arrives, give up on the scan this long after it
// started. 13 channels * 300 ms dwell plus driver overhead is ~5 s in the worst
// case; 15 s is unambiguously "something went wrong" and only ever fires as a
// recovery path, so it cannot truncate a healthy scan.
#define RECON_SCAN_WATCHDOG_MS 15000

static void schedule_restore_ap_only(void)
{
    // Owe a restore if we changed the mode *or* hopped the channel. Either one
    // leaves the SoftAP in a state main.c did not configure.
    if (!s_radioBorrowed && !s_channelBorrowed) return;
    s_restoreAtMs = now_ms() + RECON_RESTORE_SETTLE_MS;
    s_restorePending = true;
}

// Reads the SoftAP's own configured channel rather than trusting a constant, so
// the restore always lands where main.c actually put the AP. Falls back to
// BOARD_AP_CHANNEL only if the driver cannot answer.
static uint8_t ap_channel(void)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK && cfg.ap.channel >= 1 &&
        cfg.ap.channel <= 14) {
        return cfg.ap.channel;
    }
    return BOARD_AP_CHANNEL;
}

static bool enter_apsta(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) return false;
    if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_STA) return true;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APSTA switch failed: %s", esp_err_to_name(err));
        return false;
    }
    // From here on the SoftAP owes a restore (deferred; see schedule_restore).
    s_radioBorrowed = true;
    return true;
}

static void restore_ap_only(void)
{
    // Put the radio back on the SoftAP's channel first, then drop the station
    // interface. Reversing that order would leave a window where the AP is on
    // whatever channel the scan/enum hop ended on - which is the disturbance
    // issue #9 is about.
    uint8_t ch = ap_channel();
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "channel restore to %u failed: %s",
                 (unsigned)ch, esp_err_to_name(err));
    } else {
        s_channelBorrowed = false;
    }

    if (s_radioBorrowed) {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "mode restore to AP failed: %s", esp_err_to_name(err));
        } else {
            s_radioBorrowed = false;
            ESP_LOGI(TAG, "SoftAP restored (mode=AP, ch=%u)", (unsigned)ch);
        }
    } else {
        ESP_LOGI(TAG, "SoftAP channel restored (mode untouched, ch=%u)",
                 (unsigned)ch);
    }
}

// ── PMF status (best effort) ────────────────────────────────────────────────
// lily's Phase 2b read MFPC/MFPR out of the beacon's RSN IE during a
// promiscuous PMF sweep. That sweep hops the radio off the SoftAP's channel,
// which this build deliberately does not do (see recon.h), so the status is the
// Phase-2a heuristic: what the authmode implies. That is exactly what the REST
// contract allows ("best-effort is fine").
//
//   WPA3-Enterprise 192-bit  -> required (192-bit mode mandates PMF)
//   WPA3-PSK / WPA2-WPA3     -> capable  (PMF optional, i.e. MFPC=1 MFPR=0)
//   everything else          -> disabled (no RSN PMF advertisement)
static uint8_t pmf_status_for_authmode(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
        case WIFI_AUTH_WPA3_ENT_192:
            return RECON_PMF_REQUIRED;
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
        case WIFI_AUTH_WPA3_EXT_PSK:
        case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
        case WIFI_AUTH_WPA3_ENTERPRISE:
        case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
            return RECON_PMF_CAPABLE;
        case WIFI_AUTH_OPEN:
        case WIFI_AUTH_WEP:
        case WIFI_AUTH_WPA_PSK:
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WAPI_PSK:
        case WIFI_AUTH_OWE:
        default:
            return RECON_PMF_DISABLED;
    }
}

const char *recon_pmf_status_str(uint8_t status)
{
    switch (status) {
        case RECON_PMF_DISABLED: return "disabled";
        case RECON_PMF_CAPABLE:  return "capable";
        case RECON_PMF_REQUIRED: return "required";
        default:                 return "unknown";
    }
}

// ── Scan ────────────────────────────────────────────────────────────────────
// WIFI_EVENT_SCAN_DONE only raises a flag. Harvesting (which allocates driver
// memory and switches the mode back) happens on the tick task, never here.
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t id, void *data)
{
    (void)arg; (void)event_base; (void)data;
    if (id == WIFI_EVENT_SCAN_DONE) s_scanDoneFlag = true;
}

bool recon_start_scan(bool keepApUp)
{
    if (!s_ready || !s_ctlMutex) return false;
    if (xSemaphoreTake(s_ctlMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    if (radio_busy()) {
        xSemaphoreGive(s_ctlMutex);
        return false;
    }

    if (!keepApUp) {
        // The AP-down variant is not implemented on purpose: it tears the
        // SoftAP down and needs a restore path that hung the reference
        // firmware's main loop. Scan AP-up instead of risking the association
        // (issue #9); the caller gets a real scan either way.
        ESP_LOGW(TAG, "AP-down scan requested - not supported, scanning AP-up");
    }

    // The request is only armed here; the tick task performs the mode switch
    // and esp_wifi_scan_start() a tick or two later. That keeps every WiFi
    // control-plane call on one task, and means the C2 response is sent before
    // the radio starts hopping (which is also what lily's ack-before-sweep
    // contract promises).
    s_apCount = 0;
    s_scanPending = true;
    xSemaphoreGive(s_ctlMutex);
    ESP_LOGI(TAG, "scan requested (SoftAP kept up, deferred to recon task)");
    return true;
}

// Runs on the tick task. Performs the actual scan start.
static bool start_scan_now(void)
{
    s_scanPending = false;

    if (!ensure_recon_dir()) {
        // Scanning does not need the SD card; a missing recon dir only means
        // the result file cannot be saved. Not fatal.
        ESP_LOGW(TAG, "recon dir unavailable, scan results will not be saved");
    }

    // Scanning requires a station interface: in AP-only mode the driver rejects
    // esp_wifi_scan_start() with ESP_ERR_WIFI_MODE. Switch to APSTA only when
    // that is actually necessary, so a build that is already APSTA (or that a
    // future caller put in STA mode) needs no control-plane churn at all.
    // enter_apsta() records the borrow itself, which is what the restore path
    // keys off - there is no local flag to keep in step.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGW(TAG, "scan aborted: cannot read WiFi mode");
        return false;
    }
    if (mode == WIFI_MODE_AP) {
        if (!enter_apsta()) return false;
    }

    // show_hidden=true, active scan, dwell per channel, all channels (channel
    // 0 + empty bitmap), and - the issue #9 mitigation - home_chan_dwell_time
    // so the radio returns to the SoftAP's channel between every hop instead of
    // staying away for the whole sweep.
    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 0, .max = CFG_RECON_SCAN_DWELL_MS },
            .passive = CFG_RECON_SCAN_DWELL_MS,
        },
        // 30..150 ms is the driver's accepted range; 100 ms at home per hop.
        .home_chan_dwell_time = 100,
        .channel_bitmap = { .ghz_2_channels = 0, .ghz_5_channels = 0 },
        .coex_background_scan = false,
    };

    s_scanDoneFlag = false;
    s_scanning = true;
    s_scanStartMs = now_ms();
    // A managed scan hops the radio across every channel. Whatever the driver
    // does about returning home, the restore below puts the AP's channel back
    // explicitly - so record that the channel is in recon's hands even if no
    // mode switch was needed.
    s_channelBorrowed = true;

    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        s_scanning = false;
        // The scan never ran, so the radio never hopped, but a mode switch may
        // already have happened. Restore whatever we actually borrowed - the
        // helper decides that from the two flags, so this is correct whether
        // the failure was a mode problem or anything else.
        schedule_restore_ap_only();
        return false;
    }

    ESP_LOGI(TAG, "scan started (SoftAP kept up, home dwell 100ms)");
    return true;
}

bool recon_scanning(void) { return s_scanning || s_scanPending; }
uint32_t recon_last_scan_ms(void) { return s_lastScanMs; }
uint32_t recon_ap_count(void) { return s_apCount; }

const recon_ap_record_t *recon_ap_record(uint32_t idx)
{
    return idx < s_apCount ? &s_aps[idx] : NULL;
}

// Pulls the finished async scan into the RAM table, frees the driver's copy,
// prints what lily prints, saves the same lines to SD, and restores the SoftAP.
// Runs on the tick task.
static void harvest_scan(void)
{
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);

    s_apCount = 0;
    if (found > 0) {
        // One record at a time: esp_wifi_scan_get_ap_record() frees each entry
        // as it is handed over, so this needs a single ~130 B local instead of
        // a 4 KiB array on a 4 KiB task stack.
        wifi_ap_record_t rec;
        while (s_apCount < CFG_RECON_SCAN_MAX_APS &&
               esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
            recon_ap_record_t *r = &s_aps[s_apCount];
            memcpy(r->bssid, rec.bssid, 6);
            // rec.ssid is uint8_t[33] and may legitimately be exactly 32 bytes
            // of SSID with no terminator when the AP uses the full length, so
            // the copy is clamped to our 32-byte field before the NUL.
            size_t n = strnlen((const char *)rec.ssid, sizeof(rec.ssid));
            if (n > sizeof(r->ssid) - 1) n = sizeof(r->ssid) - 1;
            r->hidden = (n == 0);
            memcpy(r->ssid, rec.ssid, n);
            r->ssid[n] = '\0';
            r->channel = rec.primary;
            r->band5 = rec.primary >= 32;
            r->rssi = rec.rssi;
            r->authmode = (uint8_t)rec.authmode;
            r->pmfStatus = pmf_status_for_authmode(r->authmode);
            r->pmf = (r->pmfStatus == RECON_PMF_CAPABLE ||
                      r->pmfStatus == RECON_PMF_REQUIRED);
            s_apCount++;
        }
    }
    // Frees anything left in the driver's list (more APs than our table holds).
    esp_wifi_clear_ap_list();

    s_lastScanMs = now_ms();
    s_scanning = false;
    s_scanDoneFlag = false;

    ESP_LOGI(TAG, "scan done: %u AP(s) (%u reported)", (unsigned)s_apCount,
             (unsigned)found);
    for (uint32_t i = 0; i < s_apCount; i++) {
        const recon_ap_record_t *r = &s_aps[i];
        ESP_LOGI(TAG, "  " MACSTR " ch%-3u %s %4d dBm auth=%u pmf=%s %s",
                 MAC2STR(r->bssid), r->channel, r->band5 ? "5G  " : "2.4G",
                 (int)r->rssi, (unsigned)r->authmode,
                 recon_pmf_status_str(r->pmfStatus),
                 r->hidden ? "<hidden>" : r->ssid);
    }

    // Restore the SoftAP, deferred by the settling gap. The scan-done event only
    // set a flag, this ran a tick later, and even the mode switch itself waits
    // another RECON_RESTORE_SETTLE_MS so the driver's own async cleanup is
    // finished first - the layering that the reference firmware's hang root
    // cause pointed at.
    schedule_restore_ap_only();
}

// ── Station enumeration ─────────────────────────────────────────────────────
static void upsert_station(const uint8_t mac[6], int8_t rssi)
{
    for (uint32_t i = 0; i < s_staCount; i++) {
        if (memcmp(s_stas[i].mac, mac, 6) == 0) {
            s_stas[i].rssi = rssi;
            s_stas[i].lastSeenMs = now_ms();
            return;
        }
    }
    if (s_staCount >= CFG_RECON_MAX_STAS) return;  // table full - drop
    recon_sta_record_t *s = &s_stas[s_staCount];
    memcpy(s->mac, mac, 6);
    s->rssi = rssi;
    s->lastSeenMs = now_ms();
    s_staCount++;
    ESP_LOGI(TAG, "STA " MACSTR " rssi=%d dBm", MAC2STR(mac), (int)rssi);
}

bool recon_start_enum(uint32_t apIndex)
{
    if (!s_ready || !s_ctlMutex) return false;
    if (xSemaphoreTake(s_ctlMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;

    if (radio_busy() || apIndex >= s_apCount) {
        xSemaphoreGive(s_ctlMutex);
        return false;
    }

    // Snapshot the target now (the AP table cannot change while we own the
    // radio) and let the tick task do the channel hop and promiscuous enable.
    const recon_ap_record_t *target = &s_aps[apIndex];
    memcpy(s_enumTargetBssid, target->bssid, 6);
    s_enumChannel = target->channel;
    s_enumApIdx = apIndex;
    s_enumPending = true;
    xSemaphoreGive(s_ctlMutex);

    ESP_LOGI(TAG, "enum requested bssid=" MACSTR " channel=%u (deferred)",
             MAC2STR(s_enumTargetBssid), (unsigned)s_enumChannel);
    return true;
}

// Runs on the tick task: hops to the target AP's channel and arms the
// promiscuous listener. This is the one place recon deliberately moves the
// radio off the SoftAP's channel - there is no way to observe another AP's
// stations otherwise. finish_enum() puts the channel and AP-only mode back as
// soon as the dwell ends, so the excursion is bounded by
// CFG_RECON_ENUM_DWELL_MS * CFG_RECON_ENUM_MAX_ATTEMPTS (~8 s worst case).
static void start_enum_now(void)
{
    s_enumPending = false;

    // The enum hop is only legal with a station interface present; in AP-only
    // mode esp_wifi_set_channel() refuses anything outside the AP's channel.
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) {
        if (!enter_apsta()) return;
    }

    esp_err_t err = esp_wifi_set_channel(s_enumChannel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enum aborted: cannot move to channel %u (%s)",
                 (unsigned)s_enumChannel, esp_err_to_name(err));
        schedule_restore_ap_only();
        return;
    }
    // The hop succeeded, so the AP is no longer on its home channel. Record
    // that independently of whether we also changed the mode.
    s_channelBorrowed = true;

    if (!promiscuous_on_masked(WIFI_PROMIS_FILTER_MASK_DATA)) {
        ESP_LOGE(TAG, "enum aborted: promiscuous mode could not be enabled");
        schedule_restore_ap_only();
        return;
    }

    // Reset the ring and arm the new mode under the bus lock, same as the
    // capture path: the tick task drains under that lock, so resetting outside
    // it could discard a slot the consumer was about to read (tail beyond head -
    // an underflow that shows up as a runaway drain loop). Arming the mode in
    // the same critical section also means no frame is lost to the gap between
    // "reset" and "mode set".
    board_spi_lock();
    __atomic_store_n(&s_head, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tail, 0, __ATOMIC_RELAXED);
    s_ringMode = RING_STA_ENUM;
    board_spi_unlock();

    s_staCount = 0;
    s_enumStartMs = now_ms();
    s_enumAttempt = 1;
    s_enumRunning = true;

    ESP_LOGI(TAG, "enum begin bssid=" MACSTR " channel=%u",
             MAC2STR(s_enumTargetBssid), (unsigned)s_enumChannel);
}

bool recon_enum_running(void) { return s_enumRunning || s_enumPending; }
uint32_t recon_sta_count(void) { return s_staCount; }

const recon_sta_record_t *recon_sta_record(uint32_t idx)
{
    return idx < s_staCount ? &s_stas[idx] : NULL;
}

static void finish_enum(void)
{
    esp_wifi_set_promiscuous(false);
    // Drain under the lock and only then drop the mode: any frame the enum
    // matched but the tick task had not consumed yet still lands in the table.
    // Flipping s_ringMode first would make the tick loop fall through its mode
    // switch and silently discard them.
    board_spi_lock();
    while (__atomic_load_n(&s_tail, __ATOMIC_RELAXED) !=
           __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
        uint16_t tail = s_tail;
        recon_frame_t *slot = &s_ring[tail];
        uint8_t staMac[6];
        if (resolve_station_mac(slot->data, slot->len, s_enumTargetBssid,
                                staMac)) {
            upsert_station(staMac, slot->rssi);
        }
        s_tail = (uint16_t)((tail + 1) % CFG_RECON_RING_SLOTS);
    }
    s_ringMode = RING_NONE;
    board_spi_unlock();

    s_enumRunning = false;
    schedule_restore_ap_only();

    ESP_LOGI(TAG, "enum done: %u station(s) on ch%u", (unsigned)s_staCount,
             (unsigned)s_enumChannel);
    (void)s_enumApIdx;
}

// One tick of the enum dwell. If the attempt ends with 0 stations and attempts
// remain, extend for another full dwell instead of finishing - a genuine
// station was observed to be missed on the first dwell and caught on an
// immediate retry (channel settle / traffic cadence, not a logic bug).
static void tick_enum(void)
{
    if (now_ms() - s_enumStartMs < CFG_RECON_ENUM_DWELL_MS) return;

    if (s_staCount == 0 && s_enumAttempt < CFG_RECON_ENUM_MAX_ATTEMPTS) {
        s_enumAttempt++;
        s_enumStartMs = now_ms();
        ESP_LOGI(TAG, "0 stations after %ums, extending (attempt %u/%u)",
                 (unsigned)CFG_RECON_ENUM_DWELL_MS, (unsigned)s_enumAttempt,
                 (unsigned)CFG_RECON_ENUM_MAX_ATTEMPTS);
        return;
    }
    finish_enum();
}

// ── Refused by design ───────────────────────────────────────────────────────
bool recon_start_pmf_sweep_ap_down(void)
{
    // lily's PMFDOWN sweep tears the SoftAP down so esp_wifi_set_channel() can
    // reach DFS channels, then needs a full SoftAP restore. That restore is the
    // call that hung the reference firmware's main loop, and it is exactly what
    // issue #9 warns about. Permanently refused here; see recon.h.
    ESP_LOGW(TAG, "AP-down PMF sweep refused by design (SoftAP must stay up)");
    return false;
}

// ── Tick task ───────────────────────────────────────────────────────────────
static void recon_tick_task(void *arg)
{
    (void)arg;
    bool wroteBus = false;

    for (;;) {
        // 0. Execute deferred radio requests. Everything that touches the WiFi
        //    control plane happens here on this one task, never on the httpd
        //    task and never inside an event handler (issue #9 mitigation).
        if (s_scanPending) start_scan_now();
        if (s_enumPending) start_enum_now();

        // 1. Harvest a finished scan: it flips us back to AP-only.
        if (s_scanDoneFlag && s_scanning) {
            harvest_scan();
            wroteBus = true;   // the driver did SD/radio work; treat as bus traffic
        } else if (s_scanning &&
                   (now_ms() - s_scanStartMs) > RECON_SCAN_WATCHDOG_MS) {
            // WIFI_EVENT_SCAN_DONE never arrived. Without this the radio would
            // stay permanently "busy" and every later capture/scan/enum would
            // be refused for the rest of the boot. Free the radio and restore
            // the AP; the scan simply reports nothing.
            ESP_LOGW(TAG, "scan watchdog fired after %ums, abandoning scan",
                     (unsigned)RECON_SCAN_WATCHDOG_MS);
            s_scanning = false;
            s_scanDoneFlag = false;
            esp_wifi_clear_ap_list();
            s_lastScanMs = now_ms();
            schedule_restore_ap_only();
            wroteBus = true;
        }

        // 2. Drain whatever the current mode queued. Done before advancing the
        //    enum state machine below, because finishing an enum flips
        //    s_ringMode and would otherwise strand queued frames.
        //    Skipped entirely while a capture is shutting down: stop_capture()
        //    owns the tail index then (see s_captureStopping).
        if (!s_captureStopping) {
            board_spi_lock();
            uint16_t tail = __atomic_load_n(&s_tail, __ATOMIC_RELAXED);
            bool drained = false;
            ring_mode_t mode = s_ringMode;
            while (tail != __atomic_load_n(&s_head, __ATOMIC_ACQUIRE)) {
                recon_frame_t *slot = &s_ring[tail];
                if (mode == RING_CAPTURE) {
                    if (pcap_write_frame(slot->data, slot->len, slot->origLen,
                                         slot->timestampUs, slot->rssi,
                                         slot->channel)) {
                        __atomic_store_n(&s_packetCount, s_packetCount + 1,
                                         __ATOMIC_RELAXED);
                        drained = true;
                    }
                    if (slot->isEapol) {
                        ESP_LOGI(TAG, "EAPOL frame captured (%u bytes, rssi %d dBm)",
                                 (unsigned)slot->len, (int)slot->rssi);
                    }
                } else if (mode == RING_STA_ENUM) {
                    uint8_t staMac[6];
                    if (resolve_station_mac(slot->data, slot->len,
                                            s_enumTargetBssid, staMac)) {
                        upsert_station(staMac, slot->rssi);
                    }
                }
                tail = (uint16_t)((tail + 1) % CFG_RECON_RING_SLOTS);
                s_tail = tail;
            }
            board_spi_unlock();
            if (drained) wroteBus = true;
        }

        // 3. Advance the enum state machine.
        if (s_enumRunning) tick_enum();

        // 4. Run a scheduled AP-only restore once its settling gap has elapsed.
        //    Nothing else may be in flight: no scan, no enum, no drain backlog.
        if (s_restorePending && !s_scanning && !s_enumRunning &&
            !s_scanPending && !s_enumPending &&
            (int32_t)(now_ms() - s_restoreAtMs) >= 0) {
            s_restorePending = false;
            restore_ap_only();
            wroteBus = true;
        }

        // 5. Shared-bus hygiene: SD and LCD traffic clocked bits through the
        //    CS-less APA102, so restore the intended status colour once per
        //    tick that actually touched the bus.
        if (wroteBus) {
            board_led_relatch();
            wroteBus = false;
        }

        vTaskDelay(pdMS_TO_TICKS(CFG_RECON_TICK_MS));
    }
}

// ── Init ────────────────────────────────────────────────────────────────────
esp_err_t recon_init(void)
{
    if (s_ready) return ESP_OK;

    // The default STA netif is created by app_main before esp_wifi_start():
    // IDF requires it there, and creating it after the fact would leave the
    // driver unable to scan. Verified present here purely so a misordered boot
    // is loud rather than silently scan-less.
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        ESP_LOGW(TAG, "no default STA netif: scans will be refused, AP unaffected");
    }

    s_ctlMutex = xSemaphoreCreateMutex();
    if (!s_ctlMutex) {
        ESP_LOGE(TAG, "control mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan-done handler registration failed: %s",
                 esp_err_to_name(err));
        vSemaphoreDelete(s_ctlMutex);
        s_ctlMutex = NULL;
        return err;
    }

    if (xTaskCreate(recon_tick_task, "recon", CFG_RECON_TASK_STACK, NULL, 3,
                    &s_task) != pdPASS) {
        ESP_LOGE(TAG, "tick task create failed");
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                              NULL);
        vSemaphoreDelete(s_ctlMutex);
        s_ctlMutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG, "recon ready (capture on AP channel, scan AP-up, no AP-down sweep)");
    return ESP_OK;
}
