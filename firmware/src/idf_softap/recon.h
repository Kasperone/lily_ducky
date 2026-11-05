// =============================================================================
// recon.h - 802.11 recon on pure ESP-IDF: capture, scan, station enum (3c)
// =============================================================================
// Port of lily_ducky firmware/src/recon/recon.{h,cpp} with every Arduino
// dependency removed (no String, no WiFi.h, no SD.h). The behaviour is kept:
//
//   CAPTURE  promiscuous RX on the SoftAP's own channel -> libpcap file on SD.
//            The RX callback only classifies + copies into a ring; all PCAP
//            formatting and SD I/O happens on a low-priority tick task, because
//            ESP-IDF's sniffer docs are explicit that the callback runs in the
//            Wi-Fi driver task and must not do heavy per-packet work.
//   SCAN     esp_wifi_scan_start(config, block=false) with the SoftAP up
//            (WIFI_MODE_APSTA), harvested from WIFI_EVENT_SCAN_DONE. The AP is
//            restored to WIFI_MODE_AP afterwards, deferred by at least one tick
//            so nothing heavyweight ever runs inside the event handler.
//   ENUM     passively listens on one scanned AP's channel for Data frames to
//            or from its BSSID and builds a dedup MAC table.
//
// SOFTAP SAFETY (issue #9 - the win11 + Hama RTL8188EUS client must keep its
// association across a recon operation):
//   * Capture never changes the channel. It runs where the AP already is.
//   * Scan asks the driver to return to the home channel between every hop
//     (home_chan_dwell_time), so the AP is only ever briefly off-air.
//   * Scan and enum restore the AP channel and then WIFI_MODE_AP, always from
//     the tick task, never from an event handler.
//   * There is NO AP-down PMF sweep and no auto-chained PMF channel hop. lily's
//     startPmfSweepApDown() (which tears the SoftAP down and needs a restore
//     that hung its main loop) is stubbed permanently false here: no code path
//     in this tree can take the SoftAP down. PMF status is therefore the
//     best-effort authmode heuristic (WPA3 => capable), which the REST contract
//     explicitly allows ("best-effort is fine").
//
// One radio: capture, scan and enum are mutually exclusive. Every start*
// refuses while any of them is running (recon_busy()).
// =============================================================================
#ifndef C5_RECON_H
#define C5_RECON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One access point as seen by a managed scan. Field layout mirrors lily's
// Recon::ApRecord so the C2 JSON builder reads the same values.
typedef struct {
    uint8_t bssid[6];
    char    ssid[33];      // 32 chars + NUL; empty string => hidden SSID
    bool    hidden;
    uint8_t channel;       // primary channel
    bool    band5;         // false = 2.4 GHz, true = 5 GHz (derived from channel)
    int8_t  rssi;          // dBm
    uint8_t authmode;      // wifi_auth_mode_t
    bool    pmf;           // best-effort heuristic (WPA3 => PMF)
    uint8_t pmfStatus;     // recon_pmf_status_t
} recon_ap_record_t;

// PMF status values. Mapped to the REST strings in recon_pmf_status_str():
//   disabled / capable / required / unknown
typedef enum {
    RECON_PMF_UNKNOWN = 0,
    RECON_PMF_DISABLED,
    RECON_PMF_CAPABLE,
    RECON_PMF_REQUIRED,
} recon_pmf_status_t;

// One associated station as seen by the passive enum pass.
typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;         // dBm at last sighting
    uint32_t lastSeenMs;   // ms since boot at last sighting
} recon_sta_record_t;

// ── Lifecycle ───────────────────────────────────────────────────────────────
// Creates the ring's synchronisation objects and starts the tick task. Must be
// called after esp_wifi_start() (the tick task assumes WiFi is up) and after
// storage_init() (captures need /sdcard). Idempotent.
esp_err_t recon_init(void);

// ── Capture (/recon on the SD card) ─────────────────────────────────────────
// Starts promiscuous capture on the SoftAP's current channel, writing to
// /sdcard/recon/recon_<ms>.pcap. Fails if the radio is busy, the SD card is not
// mounted, the directory cannot be created, or the file cannot be opened.
// `outName` receives the BASENAME (matching lily and the payload API
// convention), truncated to outNameLen-1.
bool recon_start_capture(char *outName, size_t outNameLen);
void recon_stop_capture(void);
bool recon_capturing(void);
const char *recon_current_file(void);   // basename, "" when not capturing
uint32_t recon_packet_count(void);
uint32_t recon_dropped_count(void);

// ── Radio ownership ─────────────────────────────────────────────────────────
// True while capture, scan or enum owns the radio. Everything that starts a new
// mode refuses while this is true - there is one radio.
bool recon_busy(void);

// ── AP scan ─────────────────────────────────────────────────────────────────
// Kicks off an async managed scan. `keepApUp` is honoured only as true: the
// AP-down variant is deliberately not implemented (see the file header), so a
// false argument logs a warning and scans AP-up anyway rather than taking the
// SoftAP down. Returns false when the radio is busy or the driver refuses.
// Results land after ~2-5 s; poll recon_scanning(), then recon_ap_count() /
// recon_ap_record().
bool recon_start_scan(bool keepApUp);
bool recon_scanning(void);
uint32_t recon_last_scan_ms(void);       // ms at last scan completion, 0 = none
uint32_t recon_ap_count(void);
const recon_ap_record_t *recon_ap_record(uint32_t idx);  // NULL if out of range
const char *recon_pmf_status_str(uint8_t status);

// ── Station enumeration ─────────────────────────────────────────────────────
// Passively listens on apIndex's channel for CFG_RECON_ENUM_DWELL_MS (extended
// once if nothing was seen, same as lily) and builds a dedup MAC table. Fails
// if the radio is busy or apIndex is out of range.
bool recon_start_enum(uint32_t apIndex);
bool recon_enum_running(void);
uint32_t recon_sta_count(void);
const recon_sta_record_t *recon_sta_record(uint32_t idx);  // NULL if out of range

// Explicitly refused. Present so the "AP-down sweep exists but can never run"
// decision is visible in the API surface rather than an absence.
bool recon_start_pmf_sweep_ap_down(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_RECON_H
