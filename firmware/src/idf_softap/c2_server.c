// =============================================================================
// c2_server.c - LilyDucky C2 HTTP surface on plain ESP-IDF (layer 2)
// =============================================================================
// Ports the REST/dashboard routes of lily_ducky firmware/src/c2/web_server.cpp
// (Arduino WebServer) onto esp_http_server. Path, method, status code, content
// type and body shape are kept identical:
//
//   GET  /                        -> dashboard HTML (text/html)
//   GET  /raw                     -> "raw-ok\n" (text/plain)
//   GET  /api/status              -> {"state":..,"clients":N,"ap_ip":".."}
//   GET  /api/payloads            -> [{"name":"..","size":N},...]
//   GET  /api/payload/<name>      -> payload text (text/plain)
//   PUT  /api/payload/<name>      -> "OK"                            [auth]
//   POST /api/run/<name>          -> "Running"                       [auth]
//   POST /api/stop                -> "Stopped"                       [auth]
//   POST /api/recon/start         -> {"file":".."}                   [auth]
//   POST /api/recon/stop          -> "Stopped"                       [auth]
//   GET  /api/recon/status        -> {capturing,file,packets,dropped,scanning}
//   POST /api/recon/scan          -> {"scanning":true,"apUp":true}   [auth]
//   GET  /api/recon/scan          -> {scanning,lastScanMs,count,aps[]}
//   GET  /api/recon/pcap/<name>   -> capture file (application/vnd.tcpdump.pcap)
//                                                                    [auth]
//   POST /api/recon/enum/<index>  -> {"enumerating":true}            [auth]
//   GET  /api/recon/enum          -> {enumerating,count,stations[]}
//
// Everything that needs the SD card, the DuckyScript interpreter, the display
// or the recon radio goes through the c2_hooks_t function table. In layer 2
// that table is NULL, so those endpoints answer with the shape lily produces
// when the backing subsystem is unavailable (empty list, 404, 409, ...) and
// every such path is marked TODO(layer3).
// =============================================================================
#include "c2_server.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include "lily_dashboard.h"

static const char *TAG = "c2";

// Status lines. esp_http_server's default status for httpd_resp_send* is
// already "200 OK"; the rest are copied from what lily's WebServer emits.
#define C2_ST_200 "200 OK"
#define C2_ST_400 "400 Bad Request"
#define C2_ST_401 "401 Unauthorized"
#define C2_ST_404 "404 Not Found"
#define C2_ST_409 "409 Conflict"
#define C2_ST_500 "500 Internal Server Error"

#define C2_TYPE_TEXT "text/plain"
#define C2_TYPE_JSON "application/json"
#define C2_TYPE_HTML "text/html"
#define C2_TYPE_PCAP "application/vnd.tcpdump.pcap"

// Exact body lily's authOk() sends on a rejected mutating request.
#define C2_UNAUTHORIZED_MSG \
    "Unauthorized \xe2\x80\x94 supply X-Auth-Token (see serial console)"

// PUT body cap. lily buffers the body in an Arduino String with no explicit
// limit; a fixed cap keeps layer 2 from spiking the heap before the storage
// layer even exists. TODO(layer3): revisit once payload upload is real.
#define C2_MAX_BODY_LEN (16 * 1024)

// Longest path segment this module accepts, derived from lily's validName()
// rule (len < CFG_MAX_PAYLOAD_FN).
#define C2_MAX_SEGMENT (C2_MAX_PAYLOAD_FN - 1)

// ── Wiring ──────────────────────────────────────────────────────────────────
static const c2_hooks_t *s_hooks;

static char s_authToken[17];  // 16 chars + NUL, empty until generated

// ── Small growable string buffer ────────────────────────────────────────────
// Response bodies are built in RAM instead of fixed stack arrays: the recon
// tables are unbounded in an AP-dense room, and a truncated JSON body would be
// worse than none. Allocation failure marks the buffer OOM, which the caller
// turns into a 500.
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} c2_sb_t;

static void sb_init(c2_sb_t *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = false;
    // Most bodies are small; start with something that avoids a realloc for
    // the common /api/status-sized response.
    sb->cap = 128;
    sb->buf = malloc(sb->cap);
    if (!sb->buf) {
        sb->cap = 0;
        sb->oom = true;
        return;
    }
    sb->buf[0] = '\0';
}

static void sb_reserve(c2_sb_t *sb, size_t extra)
{
    if (sb->oom) return;
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t want = sb->len + extra + 1;
    size_t cap = sb->cap ? sb->cap : 128;
    while (cap < want) cap *= 2;
    char *grown = realloc(sb->buf, cap);
    if (!grown) {
        sb->oom = true;
        return;
    }
    sb->buf = grown;
    sb->cap = cap;
}

static void sb_append(c2_sb_t *sb, const char *s)
{
    if (!s) return;
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->oom) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_appendf(c2_sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        va_end(ap2);
        sb->oom = true;
        return;
    }
    sb_reserve(sb, (size_t)need);
    if (sb->oom) {
        va_end(ap2);
        return;
    }
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)need;
}

static void sb_free(c2_sb_t *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

// JSON-escape an arbitrary byte string (SSIDs are not constrained like
// payload names). Same character set lily's jsonEscape() covers.
static void sb_append_json_escaped(c2_sb_t *sb, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    for (const char *p = s; p && *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  sb_append(sb, "\\\""); break;
            case '\\': sb_append(sb, "\\\\"); break;
            case '\n': sb_append(sb, "\\n");  break;
            case '\r': sb_append(sb, "\\r");  break;
            case '\t': sb_append(sb, "\\t");  break;
            default:
                if (c < 0x20) {
                    char esc[7] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xf],
                                   hex[c & 0xf], '\0'};
                    sb_append(sb, esc);
                } else {
                    char one[2] = {(char)c, '\0'};
                    sb_append(sb, one);
                }
                break;
        }
    }
}

// ── Response helpers ────────────────────────────────────────────────────────
static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, C2_TYPE_TEXT);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_json_buf(httpd_req_t *req, c2_sb_t *sb)
{
    if (sb->oom || !sb->buf) {
        sb_free(sb);
        return send_text(req, C2_ST_500, "Out of memory");
    }
    httpd_resp_set_type(req, C2_TYPE_JSON);
    esp_err_t err = httpd_resp_sendstr(req, sb->buf);
    sb_free(sb);
    return err;
}

static esp_err_t send_json_literal(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, C2_TYPE_JSON);
    return httpd_resp_sendstr(req, json);
}

// ── SoftAP introspection (core WiFi state, not an app subsystem) ────────────
static int ap_station_count(void)
{
    wifi_sta_list_t list = {0};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return (int)list.num;
}

static void ap_ip_string(char *out, size_t outLen)
{
    // Matches lily's WiFi.softAPIP(): the AP netif address, 192.168.4.1 in the
    // stock default AP config this project keeps.
    snprintf(out, outLen, "%s", "192.168.4.1");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info = {0};
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(out, outLen, IPSTR, IP2STR(&info.ip));
    }
}

// ── Auth ────────────────────────────────────────────────────────────────────
static void generate_token(void)
{
    // Same alphabet as lily: no 0/O/1/I so the token survives being retyped
    // from the serial console.
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const uint32_t n = (uint32_t)(sizeof(alphabet) - 1);  // 32
    for (int i = 0; i < 16; i++) {
        s_authToken[i] = alphabet[esp_random() % n];
    }
    s_authToken[16] = '\0';
}

const char *c2_server_auth_token(void) { return s_authToken; }

void c2_server_set_auth_token(const char *token)
{
    if (!token) return;
    strncpy(s_authToken, token, sizeof(s_authToken) - 1);
    s_authToken[sizeof(s_authToken) - 1] = '\0';
}

// True when the request carries the current token, either as lily's
// X-Auth-Token header or as a ?token= query argument. Sends the 401 itself
// when it does not, so callers just `if (!auth_ok(req)) return ESP_OK;`.
static bool auth_ok(httpd_req_t *req)
{
    char hdr[32];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", hdr, sizeof(hdr)) == ESP_OK &&
        hdr[0] != '\0' && strcmp(hdr, s_authToken) == 0) {
        return true;
    }

    char query[128];
    char val[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", val, sizeof(val)) == ESP_OK &&
        strcmp(val, s_authToken) == 0) {
        return true;
    }

    send_text(req, C2_ST_401, C2_UNAUTHORIZED_MSG);
    return false;
}

// ── Path argument extraction ────────────────────────────────────────────────
// Lily's WebServer strips the query string before matching and does NOT
// percent-decode the path, so a path arg is the raw bytes between the prefix
// and '?'. `segment` must hold C2_MAX_SEGMENT + 1 bytes.
static bool path_segment(httpd_req_t *req, const char *prefix, char *segment,
                         size_t segmentLen, bool *hasSlash)
{
    if (hasSlash) *hasSlash = false;
    const char *uri = req->uri;
    size_t plen = strlen(prefix);
    if (strncmp(uri, prefix, plen) != 0) return false;

    const char *arg = uri + plen;
    size_t n = strcspn(arg, "?");
    if (hasSlash && memchr(arg, '/', n) != NULL) *hasSlash = true;
    if (n >= segmentLen) n = segmentLen - 1;
    memcpy(segment, arg, n);
    segment[n] = '\0';
    return true;
}

// Filename validation, mirroring lily's validName(): non-empty, shorter than
// CFG_MAX_PAYLOAD_FN, no leading dot, no separators, no "..", no control chars.
static bool valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= C2_MAX_PAYLOAD_FN) return false;
    if (name[0] == '.') return false;
    if (strstr(name, "..")) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

// Lily's UriBraces matcher refuses path args containing '/', so such a request
// never reaches a handler and falls through to onNotFound(). Reproduce that by
// answering 404 "Not found" instead of the 400 an unvalidated name would get.
static esp_err_t not_found(httpd_req_t *req)
{
    return send_text(req, C2_ST_404, "Not found");
}

// ── Body reading ────────────────────────────────────────────────────────────
static void drain_body(httpd_req_t *req, size_t len)
{
    char scratch[128];
    while (len > 0) {
        int want = (int)(len < sizeof(scratch) ? len : sizeof(scratch));
        int got = httpd_req_recv(req, scratch, want);
        if (got <= 0) break;  // timed out or socket gone; nothing more to do
        len -= (size_t)got;
    }
}

// Reads the whole request body into a malloc'd, NUL-terminated buffer.
// An empty body is valid (it yields an empty string) because lily's PUT
// handler writes whatever arg("plain") holds, which is "" for no body.
// Returns false (after draining) if the body is too large or OOM.
static bool read_body(httpd_req_t *req, char **out, size_t *outLen)
{
    *out = NULL;
    *outLen = 0;
    size_t total = req->content_len;
    if (total > C2_MAX_BODY_LEN) {
        drain_body(req, total);
        return false;
    }
    char *buf = malloc(total + 1);
    if (!buf) {
        drain_body(req, total);
        return false;
    }
    size_t done = 0;
    while (done < total) {
        int got = httpd_req_recv(req, buf + done, (int)(total - done));
        if (got <= 0) {
            free(buf);
            return false;
        }
        done += (size_t)got;
    }
    buf[done] = '\0';
    *out = buf;
    *outLen = done;
    return true;
}

// ── GET / ───────────────────────────────────────────────────────────────────
static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, C2_TYPE_HTML);
    return httpd_resp_send(req, LILY_DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

// ── GET /raw ────────────────────────────────────────────────────────────────
// lily serves this from a second esp_http_server on :81 (CFG_C2_MIN_REPRO
// instrumentation for open-questions.md #9). Same path/method/body/type here,
// on the one :80 server, so no extra httpd task or ctrl port is needed.
static esp_err_t handle_raw(httpd_req_t *req)
{
    httpd_resp_set_type(req, C2_TYPE_TEXT);
    return httpd_resp_sendstr(req, "raw-ok\n");
}

// ── GET /api/status ─────────────────────────────────────────────────────────
static const char *interp_state_string(void)
{
    // TODO(layer3): the DuckyScript interpreter supplies this via
    // c2_hooks_t::getInterpState. Without it, lily reports "no_interp".
    if (!s_hooks || !s_hooks->getInterpState) return "no_interp";
    switch (s_hooks->getInterpState()) {
        case C2_INTERP_IDLE:     return "idle";
        case C2_INTERP_RUNNING:  return "running";
        case C2_INTERP_COMPLETE: return "complete";
        case C2_INTERP_ERROR:    return "error";
        case C2_INTERP_STOPPED:  return "stopped";
        default:                 return "unknown";
    }
}

static esp_err_t handle_status(httpd_req_t *req)
{
    char ip[16];
    ap_ip_string(ip, sizeof(ip));

    c2_sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"state\":\"%s\",\"clients\":%d,\"ap_ip\":\"%s\"}",
               interp_state_string(), ap_station_count(), ip);
    return send_json_buf(req, &sb);
}

// ── GET /api/payloads ───────────────────────────────────────────────────────
static esp_err_t handle_list_payloads(httpd_req_t *req)
{
    c2_sb_t sb;
    sb_init(&sb);
    sb_append(&sb, "[");

    // TODO(layer3): walk C2_SD_PAYLOAD_DIR on the SD card through
    // c2_hooks_t::listPayloads/payloadCount/payloadName/payloadSize. With no
    // storage the directory is unavailable and lily answers an empty array,
    // which is what this does.
    if (s_hooks && s_hooks->listPayloads && s_hooks->listPayloads()) {
        int count = s_hooks->payloadCount ? s_hooks->payloadCount() : 0;
        for (int i = 0; i < count; i++) {
            const char *name = s_hooks->payloadName ? s_hooks->payloadName(i) : NULL;
            if (!name) break;
            if (i) sb_append(&sb, ",");
            sb_append(&sb, "{\"name\":\"");
            sb_append_json_escaped(&sb, name);
            sb_appendf(&sb, "\",\"size\":%u}",
                       (unsigned)(s_hooks->payloadSize ? s_hooks->payloadSize(i) : 0));
        }
    }

    sb_append(&sb, "]");
    return send_json_buf(req, &sb);
}

// ── GET /api/payload/<name> ─────────────────────────────────────────────────
static esp_err_t handle_get_payload(httpd_req_t *req)
{
    char name[C2_MAX_SEGMENT + 1];
    bool hasSlash = false;
    if (!path_segment(req, "/api/payload/", name, sizeof(name), &hasSlash) || hasSlash) {
        return not_found(req);
    }
    if (!valid_name(name)) {
        return send_text(req, C2_ST_400, "Invalid name");
    }

    // TODO(layer3): read C2_SD_PAYLOAD_DIR/<name> from the SD card. With no
    // storage the file cannot exist, and lily answers 404.
    char *payload = NULL;
    size_t payloadLen = 0;
    if (s_hooks && s_hooks->readPayload &&
        s_hooks->readPayload(name, &payload, &payloadLen)) {
        httpd_resp_set_type(req, C2_TYPE_TEXT);
        esp_err_t err = httpd_resp_send(req, payload, (ssize_t)payloadLen);
        free(payload);
        return err;
    }
    return send_text(req, C2_ST_404, "Not found");
}

// ── PUT /api/payload/<name> ─────────────────────────────────────────────────
static esp_err_t handle_put_payload(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    char name[C2_MAX_SEGMENT + 1];
    bool hasSlash = false;
    if (!path_segment(req, "/api/payload/", name, sizeof(name), &hasSlash) || hasSlash) {
        drain_body(req, req->content_len);
        return not_found(req);
    }
    if (!valid_name(name)) {
        drain_body(req, req->content_len);
        return send_text(req, C2_ST_400, "Invalid name");
    }

    char *body = NULL;
    size_t bodyLen = 0;
    bool haveBody = read_body(req, &body, &bodyLen);

    // TODO(layer3): persist the body to C2_SD_PAYLOAD_DIR/<name>. Until the
    // SD card exists the write cannot succeed, which is lily's 500 "Cannot
    // write" path.
    bool written = false;
    if (s_hooks && s_hooks->writePayload && haveBody) {
        written = s_hooks->writePayload(name, body, bodyLen);
    }
    free(body);

    if (!written) {
        return send_text(req, C2_ST_500, "Cannot write");
    }
    return send_text(req, C2_ST_200, "OK");
}

// ── POST /api/run/<name> ────────────────────────────────────────────────────
static esp_err_t handle_run_payload(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    char name[C2_MAX_SEGMENT + 1];
    bool hasSlash = false;
    if (!path_segment(req, "/api/run/", name, sizeof(name), &hasSlash) || hasSlash) {
        return not_found(req);
    }
    if (!valid_name(name)) {
        return send_text(req, C2_ST_400, "Invalid name");
    }

    // TODO(layer3): load C2_SD_PAYLOAD_DIR/<name> and hand it to the
    // interpreter. With neither storage nor interpreter present the load
    // fails, which is lily's 404 "Payload not found".
    if (!s_hooks || !s_hooks->runPayload || !s_hooks->runPayload(name)) {
        return send_text(req, C2_ST_404, "Payload not found");
    }
    return send_text(req, C2_ST_200, "Running");
}

// ── POST /api/stop ──────────────────────────────────────────────────────────
static esp_err_t handle_stop(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    // TODO(layer3): stop the running DuckyScript payload.
    if (s_hooks && s_hooks->stopPayload) s_hooks->stopPayload();
    return send_text(req, C2_ST_200, "Stopped");
}

// ── POST /api/recon/start ───────────────────────────────────────────────────
static esp_err_t handle_recon_start(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    // TODO(layer3): start a PCAP capture on the AP's channel into
    // C2_SD_RECON_DIR. Without a capture engine / SD card the start fails,
    // which is lily's 409 path.
    char file[40] = {0};
    if (!s_hooks || !s_hooks->reconStartCapture ||
        !s_hooks->reconStartCapture(file, sizeof(file))) {
        return send_text(req, C2_ST_409, "Capture already running or SD unavailable");
    }

    c2_sb_t sb;
    sb_init(&sb);
    sb_append(&sb, "{\"file\":\"");
    sb_append_json_escaped(&sb, file);
    sb_append(&sb, "\"}");
    return send_json_buf(req, &sb);
}

// ── POST /api/recon/stop ────────────────────────────────────────────────────
static esp_err_t handle_recon_stop(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;
    // TODO(layer3): stop the PCAP capture and close the file.
    if (s_hooks && s_hooks->reconStopCapture) s_hooks->reconStopCapture();
    return send_text(req, C2_ST_200, "Stopped");
}

// ── GET /api/recon/status ───────────────────────────────────────────────────
static esp_err_t handle_recon_status(httpd_req_t *req)
{
    // TODO(layer3): real capture counters and scan state. Layer 2 reports the
    // idle shape.
    bool capturing = s_hooks && s_hooks->reconCapturing && s_hooks->reconCapturing();
    const char *file = (s_hooks && s_hooks->reconCurrentFile) ? s_hooks->reconCurrentFile() : "";
    uint32_t packets = (s_hooks && s_hooks->reconPacketCount) ? s_hooks->reconPacketCount() : 0;
    uint32_t dropped = (s_hooks && s_hooks->reconDroppedCount) ? s_hooks->reconDroppedCount() : 0;
    bool scanning = s_hooks && s_hooks->reconScanning && s_hooks->reconScanning();

    c2_sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"capturing\":%s,\"file\":\"", capturing ? "true" : "false");
    sb_append_json_escaped(&sb, file ? file : "");
    sb_appendf(&sb, "\",\"packets\":%u,\"dropped\":%u,\"scanning\":%s}",
               (unsigned)packets, (unsigned)dropped, scanning ? "true" : "false");
    return send_json_buf(req, &sb);
}

// ── POST /api/recon/scan ────────────────────────────────────────────────────
static esp_err_t handle_recon_scan_start(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    if (s_hooks && s_hooks->reconBusy && s_hooks->reconBusy()) {
        return send_text(req, C2_ST_409, "Recon radio already busy");
    }

    // TODO(layer3): kick off Recon::startScan(true) (dual-band managed scan
    // with the SoftAP kept up, ack sent before the sweep starts). Layer 2
    // returns the same ack shape but runs nothing, so GET /api/recon/scan
    // immediately reports scanning:false with an empty table.
    if (s_hooks && s_hooks->reconStartScan) s_hooks->reconStartScan(true);

    return send_json_literal(req, "{\"scanning\":true,\"apUp\":true}");
}

// ── GET /api/recon/scan ─────────────────────────────────────────────────────
static esp_err_t handle_recon_scan_results(httpd_req_t *req)
{
    bool scanning = s_hooks && s_hooks->reconScanning && s_hooks->reconScanning();
    uint32_t lastMs = (s_hooks && s_hooks->reconLastScanMs) ? s_hooks->reconLastScanMs() : 0;
    uint32_t count = (s_hooks && s_hooks->reconApCount) ? s_hooks->reconApCount() : 0;

    c2_sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"scanning\":%s,\"lastScanMs\":%u,\"count\":%u,\"aps\":[",
               scanning ? "true" : "false", (unsigned)lastMs, (unsigned)count);

    // TODO(layer3): fill rows from Recon::apRecord(). Layer 2 has no scan
    // engine, so the table is empty exactly as it is in lily before a scan.
    if (s_hooks && s_hooks->reconApRecord) {
        for (uint32_t i = 0; i < count; i++) {
            char bssid[18];
            char ssid[33];
            bool hidden = false;
            uint8_t channel = 0;
            bool band5 = false;
            int8_t rssi = 0;
            uint8_t authmode = 0;
            bool pmf = false;
            const char *pmfStatus = NULL;
            if (!s_hooks->reconApRecord(i, bssid, sizeof(bssid), ssid, sizeof(ssid),
                                        &hidden, &channel, &band5, &rssi,
                                        &authmode, &pmf, &pmfStatus)) {
                break;
            }
            if (i) sb_append(&sb, ",");
            sb_append(&sb, "{\"bssid\":\"");
            sb_append_json_escaped(&sb, bssid);
            sb_append(&sb, "\",\"ssid\":\"");
            sb_append_json_escaped(&sb, hidden ? "" : ssid);
            sb_appendf(&sb, "\",\"hidden\":%s,\"channel\":%u,\"band\":\"%s\","
                            "\"rssi\":%d,\"authmode\":%u,\"pmf\":%s,"
                            "\"pmfStatus\":\"%s\"}",
                       hidden ? "true" : "false", (unsigned)channel,
                       band5 ? "5" : "2.4", (int)rssi, (unsigned)authmode,
                       pmf ? "true" : "false",
                       pmfStatus ? pmfStatus : "unknown");
        }
    }

    sb_append(&sb, "]}");
    return send_json_buf(req, &sb);
}

// ── GET /api/recon/pcap/<name> ──────────────────────────────────────────────
static esp_err_t handle_recon_get_pcap(httpd_req_t *req)
{
    // Auth-gated unlike GET /api/payload/<name>: a capture can hold an EAPOL
    // handshake, which is more sensitive than a payload script.
    if (!auth_ok(req)) return ESP_OK;

    char name[C2_MAX_SEGMENT + 1];
    bool hasSlash = false;
    if (!path_segment(req, "/api/recon/pcap/", name, sizeof(name), &hasSlash) || hasSlash) {
        return not_found(req);
    }
    if (!valid_name(name)) {
        return send_text(req, C2_ST_400, "Invalid name");
    }

    // Load the capture into RAM and send it in one response, the same way
    // handle_get_payload() does. Captures here are small lab snippets, and
    // lily's history is explicit that the NODELAY + tiny-chunk send loop was
    // actively counterproductive: a plain send lets TCP batch normally.
    uint8_t *buf = NULL;
    size_t len = 0;
    if (s_hooks && s_hooks->reconReadPcap &&
        s_hooks->reconReadPcap(name, &buf, &len)) {
        httpd_resp_set_type(req, C2_TYPE_PCAP);
        esp_err_t err = httpd_resp_send(req, (const char *)buf, (ssize_t)len);
        free(buf);
        return err;
    }
    // No storage, no such file: lily's 404.
    return send_text(req, C2_ST_404, "Not found");
}

// ── POST /api/recon/enum/<index> ────────────────────────────────────────────
static esp_err_t handle_recon_enum_start(httpd_req_t *req)
{
    if (!auth_ok(req)) return ESP_OK;

    char idxStr[C2_MAX_SEGMENT + 1];
    bool hasSlash = false;
    if (!path_segment(req, "/api/recon/enum/", idxStr, sizeof(idxStr), &hasSlash) || hasSlash) {
        return not_found(req);
    }

    // lily does `pathArg(0).toInt()`: non-numeric text yields 0, a negative
    // index is rejected, and startEnum() fails when no scan table exists.
    long idx = strtol(idxStr, NULL, 10);
    // TODO(layer3): Recon::startEnum((uint32_t)idx) passive station sniff.
    if (idx < 0 || !s_hooks || !s_hooks->reconStartEnum ||
        !s_hooks->reconStartEnum((uint32_t)idx)) {
        return send_text(req, C2_ST_409, "Recon radio busy or index out of range");
    }
    return send_json_literal(req, "{\"enumerating\":true}");
}

// ── GET /api/recon/enum ─────────────────────────────────────────────────────
static esp_err_t handle_recon_enum_results(httpd_req_t *req)
{
    bool running = s_hooks && s_hooks->reconEnumRunning && s_hooks->reconEnumRunning();
    uint32_t count = (s_hooks && s_hooks->reconStaCount) ? s_hooks->reconStaCount() : 0;

    c2_sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb, "{\"enumerating\":%s,\"count\":%u,\"stations\":[",
               running ? "true" : "false", (unsigned)count);

    // TODO(layer3): fill rows from Recon::staRecord().
    if (s_hooks && s_hooks->reconStaRecord) {
        for (uint32_t i = 0; i < count; i++) {
            char mac[18];
            int8_t rssi = 0;
            uint32_t lastSeenMs = 0;
            if (!s_hooks->reconStaRecord(i, mac, sizeof(mac), &rssi, &lastSeenMs)) break;
            if (i) sb_append(&sb, ",");
            sb_append(&sb, "{\"mac\":\"");
            sb_append_json_escaped(&sb, mac);
            sb_appendf(&sb, "\",\"rssi\":%d,\"lastSeenMs\":%u}", (int)rssi,
                       (unsigned)lastSeenMs);
        }
    }

    sb_append(&sb, "]}");
    return send_json_buf(req, &sb);
}

// ── Error handlers ──────────────────────────────────────────────────────────
// lily's onNotFound() answers every unrouted URI *and* every wrong-method
// request with 404 "Not found"; esp_http_server would answer 405 for the
// latter, so both error codes are redirected to the same body.
static esp_err_t handle_err_not_found(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    return send_text(req, C2_ST_404, "Not found");
}

// ── Registration ────────────────────────────────────────────────────────────
static const httpd_uri_t s_routes[] = {
    {.uri = "/",                  .method = HTTP_GET,  .handler = handle_root},
    {.uri = "/raw",               .method = HTTP_GET,  .handler = handle_raw},
    {.uri = "/api/status",        .method = HTTP_GET,  .handler = handle_status},
    {.uri = "/api/payloads",      .method = HTTP_GET,  .handler = handle_list_payloads},
    {.uri = "/api/payload/*",     .method = HTTP_GET,  .handler = handle_get_payload},
    {.uri = "/api/payload/*",     .method = HTTP_PUT,  .handler = handle_put_payload},
    {.uri = "/api/run/*",         .method = HTTP_POST, .handler = handle_run_payload},
    {.uri = "/api/stop",          .method = HTTP_POST, .handler = handle_stop},
    {.uri = "/api/recon/start",   .method = HTTP_POST, .handler = handle_recon_start},
    {.uri = "/api/recon/stop",    .method = HTTP_POST, .handler = handle_recon_stop},
    {.uri = "/api/recon/status",  .method = HTTP_GET,  .handler = handle_recon_status},
    {.uri = "/api/recon/scan",    .method = HTTP_POST, .handler = handle_recon_scan_start},
    {.uri = "/api/recon/scan",    .method = HTTP_GET,  .handler = handle_recon_scan_results},
    {.uri = "/api/recon/pcap/*",  .method = HTTP_GET,  .handler = handle_recon_get_pcap},
    {.uri = "/api/recon/enum/*",  .method = HTTP_POST, .handler = handle_recon_enum_start},
    {.uri = "/api/recon/enum",    .method = HTTP_GET,  .handler = handle_recon_enum_results},
};

// Keep the advertised slot count honest: a mismatch would silently drop routes
// at registration time (httpd_register_uri_handler returns HANDLERS_FULL).
_Static_assert(sizeof(s_routes) / sizeof(s_routes[0]) == C2_URI_HANDLER_COUNT,
               "C2_URI_HANDLER_COUNT must match the route table size");

esp_err_t c2_server_register(httpd_handle_t server, const c2_hooks_t *hooks)
{
    if (!server) return ESP_ERR_INVALID_ARG;

    s_hooks = hooks;

    if (s_authToken[0] == '\0') {
        generate_token();
        ESP_LOGI(TAG, "Auth token: %s", s_authToken);
        ESP_LOGI(TAG, "Mutating routes require this in the X-Auth-Token header");
    }

    const size_t n = sizeof(s_routes) / sizeof(s_routes[0]);
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s (method %d) not registered: %s", s_routes[i].uri,
                     (int)s_routes[i].method, esp_err_to_name(err));
            return err;
        }
    }

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_err_not_found);
    httpd_register_err_handler(server, HTTPD_405_METHOD_NOT_ALLOWED, handle_err_not_found);

    ESP_LOGI(TAG, "C2 routes registered: %u handlers", (unsigned)n);
    return ESP_OK;
}
