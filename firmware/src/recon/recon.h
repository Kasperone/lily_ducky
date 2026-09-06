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

    // Drains the ring buffer into the open PCAP file. Call every loop().
    void tick();

    // Status for the REST API / LCD.
    uint32_t packetCount();
    uint32_t droppedCount();    // frames dropped because the ring was full
    const char* currentFile();  // empty string when not capturing

    // ── Phase 2a: dual-band AP scan / enumeration ──────────────────────────
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

} // namespace Recon

#endif
