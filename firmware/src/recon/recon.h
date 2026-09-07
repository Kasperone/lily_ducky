// =============================================================================
// recon/recon.h — WiFi 802.11 promiscuous capture -> PCAP on SD (Module B)
// =============================================================================
// Phase 1 scope: capture management + EAPOL data frames on the C2 SoftAP's
// own channel while the SoftAP stays up — one radio, the AP already pins the
// channel, so this deliberately does not hop channels (see config.h). The
// promiscuous RX callback only copies matching frames into a ring buffer;
// all PCAP formatting and SD I/O happens in tick(), because ESP-IDF's own
// Wi-Fi Sniffer Mode docs say the callback runs directly in the Wi-Fi driver
// task and heavy per-packet work must be deferred to the application task
// (docs.espressif.com esp-idf api-guides/wifi-driver/wifi-modes).
// =============================================================================
#ifndef LILY_DUCKY_RECON_H
#define LILY_DUCKY_RECON_H

#include <Arduino.h>

namespace Recon {

    // Starts promiscuous capture on the SoftAP's current channel, writing to
    // SD_RECON_DIR/<name>. Fails if SD isn't ready, a capture is already
    // running, or the file can't be opened. `outPath` (if non-null) is
    // filled with the full path actually used.
    bool startCapture(char* outPath, size_t outPathLen);
    void stopCapture();
    bool capturing();

    // True while any mode (capture, scan, PMF sweep, or station enum) owns
    // the radio — everything that starts a new mode should refuse while this
    // is true, since there is only one radio.
    bool busy();

    // Drains the ring buffer into the open PCAP file. Call every loop().
    void tick();

    // Status for the REST API / LCD.
    uint32_t packetCount();
    uint32_t droppedCount();    // frames dropped because the ring was full
    const char* currentFile();  // empty string when not capturing

    // ── Phase 2a: dual-band AP scan / enumeration ──────────────────────────
    // Real PMF status parsed from a beacon/probe-resp RSN IE (Phase 2b).
    // UNKNOWN until a PMF sweep has actually seen and parsed this BSSID's
    // RSN IE; falls back to ApRecord::pmf's authmode heuristic until then.
    enum class PmfStatus : uint8_t { UNKNOWN = 0, NOT_PROTECTED = 1, CAPABLE = 2, REQUIRED = 3 };

    // One access point as seen by a managed scan.
    struct ApRecord {
        uint8_t bssid[6];
        char    ssid[33];       // 32 chars + NUL; empty string => hidden SSID
        bool    hidden;
        uint8_t channel;        // primary channel
        bool    band5;          // false = 2.4 GHz, true = 5 GHz (derived from channel)
        int8_t  rssi;           // dBm
        uint8_t authmode;       // wifi_auth_mode_t
        bool    pmf;            // best-effort in 2a (WPA3 => PMF); reliable parse is 2b
        PmfStatus pmfStatus;    // 2b: real MFPC/MFPR from the RSN IE; UNKNOWN until swept
    };

    // One associated station as seen by Phase 2b's passive per-AP enum pass.
    struct StaRecord {
        uint8_t  mac[6];
        int8_t   rssi;          // dBm at last sighting
        uint32_t lastSeenMs;    // millis() at last sighting
    };

    // Kicks off an async managed scan across the channels the current band
    // allows. keepApUp=true (default) scans with the SoftAP up (APSTA) so the
    // dashboard stays live — brief client blips as the radio hops. keepApUp=
    // false tears the SoftAP down for an unconstrained dual-band sweep and
    // restores it afterward. Fails if a capture or scan is already running.
    // Results land after ~1-3 s; poll scanning(), then read apCount()/apRecord().
    bool startScan(bool keepApUp = true);
    bool scanning();                       // true while a sweep is in flight
    uint32_t apCount();                    // APs found by the last completed scan
    const ApRecord* apRecord(uint32_t i);  // nullptr if i is out of range
    uint32_t lastScanMillis();             // millis() at last scan completion (0 = none)

    // ── Phase 2b: RSN-IE PMF sweep + station enumeration ────────────────────
    // Sweeps the channels from the last completed scan's AP table, hopping
    // each to catch a beacon/probe-resp and parse its RSN IE. Auto-chained
    // after startScan() only when CFG_RECON_AUTO_PMF_SWEEP is set (default
    // OFF — see config.h); otherwise call this explicitly (console `PMF`
    // command). Fails if the radio is busy or the last scan found no APs.
    // pmfSweeping() polls it; results land in ApRecord::pmfStatus in place.
    bool startPmfSweep();
    bool pmfSweeping();

    // Station enum is manual: pass the index of an AP from the last scan
    // (apRecord(i)). Passively listens on that AP's channel for Data frames
    // to/from its BSSID for CFG_RECON_ENUM_DWELL_MS and builds a dedup table
    // of the station MACs seen. Fails if the radio is busy (capture/scan/
    // sweep/another enum already running) or apIndex is out of range.
    bool startEnum(uint32_t apIndex);
    bool enumRunning();
    uint32_t staCount();                     // stations found by the last completed enum
    const StaRecord* staRecord(uint32_t i);  // nullptr if i is out of range

    // ── AP-down DFS-capable PMF sweep (investigation-gated, console
    // `PMFDOWN` only — never auto-triggered; SCAN/PMF's AP-up paths are
    // completely unaffected). Tears the SoftAP down, sweeps every channel
    // the last scan found (including DFS channels the AP-up PMF sweep can't
    // reach), then signals for the SoftAP to be restored. Deferred/tick-
    // based throughout — see recon.cpp's tickApDownSweep() for the full
    // phase sequence and config.h for the settling-delay rationale.
    //
    // Recon deliberately does NOT depend on c2/web_server.h (one-way
    // dependency: c2 depends on recon, not the reverse), so it cannot call
    // the actual SoftAP restore (C2Server::restartSoftAp()) itself. Instead:
    // main.cpp's loop() must poll apRestorePending() every tick and, when
    // true, call C2Server::restartSoftAp() itself (the one never-fully-
    // proven call — this is where a hang would manifest, same as before)
    // and then call notifyApRestored() so Recon can finish its own sequence.
    bool startPmfSweepApDown();
    bool apRestorePending();  // true for main.cpp to act on; see above
    void notifyApRestored();  // main.cpp calls this right after restartSoftAp() returns

} // namespace Recon

#endif
