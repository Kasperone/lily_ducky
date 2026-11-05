// =============================================================================
// c2_server.h - LilyDucky C2 HTTP surface on plain ESP-IDF (layer 2)
// =============================================================================
// Re-implements the lily_ducky C2 REST API on esp_http_server. Paths, methods,
// status codes, content types and body shapes mirror firmware/src/c2/
// web_server.cpp (Arduino WebServer) so higher layers can drop in unchanged.
//
// The SoftAP itself stays owned by main.c (known-good bring-up); this module
// only registers URI handlers on an already-started httpd handle.
// =============================================================================
#ifndef C2_SERVER_H
#define C2_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── Wire-level limits shared with lily's config.h ───────────────────────────
// CFG_MAX_PAYLOAD_FN (48) caps payload/capture filenames; SD_PAYLOAD_DIR and
// SD_RECON_DIR are the on-card directory names that the layer-3 storage
// backend will use. Kept here so this layer carries no SD dependency.
#define C2_MAX_PAYLOAD_FN 48
#define C2_SD_PAYLOAD_DIR "/payloads"
#define C2_SD_RECON_DIR   "/recon"

// Interpreter states reported by GET /api/status. Values match lily's
// InterpState ordering so the JSON strings agree.
typedef enum {
    C2_INTERP_IDLE = 0,
    C2_INTERP_RUNNING,
    C2_INTERP_COMPLETE,
    C2_INTERP_ERROR,
    C2_INTERP_STOPPED,
} c2_interp_state_t;

// App-subsystem hooks. Every one is NULL in layer 2, which makes the affected
// endpoints answer their "nothing available yet" shape instead of touching
// hardware. Layer 3 fills these in; no handler changes are needed then.
typedef struct {
    // GET /api/status -> "state". NULL pointer means the interpreter is not
    // wired up at all ("no_interp", as in lily before setInterpreter()).
    c2_interp_state_t (*getInterpState)(void);

    // Payload directory (/payloads on the SD card).
    // GET /api/payloads walks it; GET/PUT /api/payload/<name> read/write files.
    bool (*listPayloads)(void);                       // true if the listing below is valid
    int  (*payloadCount)(void);
    const char *(*payloadName)(int idx);
    size_t (*payloadSize)(int idx);
    bool (*readPayload)(const char *name, char **outBuf, size_t *outLen);  // malloc'd
    bool (*writePayload)(const char *name, const char *body, size_t bodyLen);

    // POST /api/run/<name>: load the payload and start the interpreter.
    bool (*runPayload)(const char *name);
    // POST /api/stop.
    void (*stopPayload)(void);

    // Recon capture (/recon on the SD card).
    bool (*reconStartCapture)(char *outName, size_t outNameLen);
    void (*reconStopCapture)(void);
    bool (*reconCapturing)(void);
    const char *(*reconCurrentFile)(void);
    uint32_t (*reconPacketCount)(void);
    uint32_t (*reconDroppedCount)(void);
    // GET /api/recon/pcap/<name>: loads /recon/<name> and hands back the exact
    // binary byte count (libpcap data, so no NUL terminator is expected or
    // appended). *outBuf is malloc'd on success and freed by the handler.
    // A false return (no card, no file, unsafe name) becomes the 404.
    bool (*reconReadPcap)(const char *name, uint8_t **outBuf, size_t *outLen);

    // Recon AP scan.
    bool (*reconBusy)(void);
    bool (*reconStartScan)(bool keepApUp);
    bool (*reconScanning)(void);
    uint32_t (*reconLastScanMs)(void);
    uint32_t (*reconApCount)(void);
    // Fills one AP row. pmfStatus is one of "disabled"/"capable"/"required"/
    // "unknown" (NULL -> "unknown").
    bool (*reconApRecord)(uint32_t idx, char *bssid, size_t bssidLen,
                          char *ssid, size_t ssidLen, bool *hidden,
                          uint8_t *channel, bool *band5, int8_t *rssi,
                          uint8_t *authmode, bool *pmf, const char **pmfStatus);

    // Recon station enumeration.
    bool (*reconStartEnum)(uint32_t idx);
    bool (*reconEnumRunning)(void);
    uint32_t (*reconStaCount)(void);
    bool (*reconStaRecord)(uint32_t idx, char *mac, size_t macLen,
                           int8_t *rssi, uint32_t *lastSeenMs);
} c2_hooks_t;

// Register every C2 route on an already-started httpd. `hooks` may be NULL
// (equivalent to an all-NULL table). Returns ESP_OK once all handlers are
// registered. The httpd config must allow at least C2_URI_HANDLER_COUNT
// handlers - see C2_URI_HANDLER_COUNT.
esp_err_t c2_server_register(httpd_handle_t server, const c2_hooks_t *hooks);

// Number of URI handler slots c2_server_register() consumes (both methods of
// a shared path count separately). httpd_config_t::max_uri_handlers default
// is 8, so the caller raises it.
#define C2_URI_HANDLER_COUNT 16

// Per-boot C2 auth token (16 chars, printed to the console at boot). Required
// in the X-Auth-Token header - or a ?token= query arg - by mutating routes,
// exactly like lily's authOk().
const char *c2_server_auth_token(void);
void c2_server_set_auth_token(const char *token);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C2_SERVER_H
