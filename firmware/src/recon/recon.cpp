// =============================================================================
// recon/recon.cpp — WiFi 802.11 promiscuous capture -> PCAP on SD (Module B)
// =============================================================================
#include "recon.h"
#include <esp_wifi.h>
#include <WiFi.h>
#include "config.h"
#include "storage/storage.h"
#include "pcap_writer.h"

// ── Ring buffer (SPSC: promiscuous callback is the sole producer, tick() the
//    sole consumer) ─────────────────────────────────────────────────────────
// The C5 is single-core (AGENTS.md), so a plain volatile head/tail index pair
// is sufficient here — no cross-core cache coherency to worry about, only
// preemption ordering between the Wi-Fi driver task (producer) and the main
// loop() task (consumer), which volatile already covers for this pattern.
struct ReconFrame {
    uint8_t  data[CFG_RECON_SNAPLEN];
    uint16_t len;          // captured length (<= CFG_RECON_SNAPLEN)
    uint16_t origLen;      // true on-air length (rx_ctrl.sig_len), may exceed len
    uint32_t timestampUs;
    int8_t   rssi;
    uint8_t  channel;
    bool     isEapol;      // true for the WIFI_PKT_DATA frames (all of which
                            // are EAPOL by construction — see promiscuousCb)
};

static ReconFrame _ring[CFG_RECON_RING_SLOTS];
static volatile uint16_t _head = 0;
static volatile uint16_t _tail = 0;
static volatile uint32_t _dropped = 0;
static uint32_t _packetCount = 0;
static char _currentFile[40] = "";  // basename only (matches the payload API's convention)
static bool _capturing = false;

// ── Phase 2a scan state ─────────────────────────────────────────────────────
// The managed scan (WiFi.scanNetworks) is async; tick() harvests the result
// once WiFi.scanComplete() reports done. Scan and capture are mutually
// exclusive (one radio) — both start functions refuse if the other is active.
static Recon::ApRecord _aps[CFG_RECON_SCAN_MAX_APS];
static uint32_t _apCount = 0;
static bool _scanning = false;
static uint32_t _lastScanMs = 0;

// Data frame -> EAPOL check: standard 802.11 QoS-Data header (24B, +2B QoS
// Control when the subtype's QoS bit is set) followed by an 802.2 LLC/SNAP
// header (AA AA 03 00-00-00 <ethertype:2>) with EtherType 0x888E. This is
// plain protocol decoding, not chip-specific. Not handled: WDS (+Addr4) and
// +HTC/Order-bit frames, neither of which our own AP/STA lab traffic uses —
// noted as a known gap rather than silently mishandled.
static bool isEapolDataFrame(const uint8_t* f, uint16_t len)
{
    if (len < 24) return false;
    uint8_t fc0 = f[0];
    uint8_t type = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0xF;
    if (type != 2) return false; // not a Data frame

    int hdrLen = 24;
    if (subtype & 0x08) hdrLen += 2; // QoS Data subtypes carry a QoS Control field
    if (len < (uint16_t)(hdrLen + 8)) return false;

    const uint8_t* llc = f + hdrLen;
    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
        llc[3] == 0x00 && llc[4] == 0x00 && llc[5] == 0x00) {
        uint16_t etherType = ((uint16_t)llc[6] << 8) | llc[7];
        return etherType == 0x888E; // EAPOL
    }
    return false;
}

// Runs in the Wi-Fi driver task (see recon.h) — kept to cheap branches plus
// one bounded memcpy. No Serial, no SD, no malloc: all deferred to tick().
static void promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t origLen = pkt->rx_ctrl.sig_len;

    // Data frames: only keep ones that decode as EAPOL, so ordinary client
    // traffic (the Win11 VM's normal browsing, C2 dashboard HTTP, etc.)
    // never enters the ring. Management frames (beacon/probe/auth/assoc/
    // deauth) are all kept — that's the AP/station enumeration data.
    if (type == WIFI_PKT_DATA && !isEapolDataFrame(frame, origLen)) return;

    uint16_t next = (_head + 1) % CFG_RECON_RING_SLOTS;
    if (next == _tail) { _dropped = _dropped + 1; return; } // ring full — drop, don't block

    ReconFrame& slot = _ring[_head];
    uint16_t copyLen = origLen < CFG_RECON_SNAPLEN ? origLen : CFG_RECON_SNAPLEN;
    memcpy(slot.data, frame, copyLen);
    slot.len = copyLen;
    slot.origLen = origLen;
    slot.timestampUs = pkt->rx_ctrl.timestamp;
    slot.rssi = pkt->rx_ctrl.rssi;
    slot.channel = pkt->rx_ctrl.channel;
    slot.isEapol = (type == WIFI_PKT_DATA); // only EAPOL-decoded data frames reach here
    _head = next;
}

bool Recon::startCapture(char* outPath, size_t outPathLen)
{
    if (_capturing || _scanning) return false;   // one radio — no overlap
    if (!Storage::ready()) return false;

    if (!Storage::dirExists(SD_RECON_DIR) && !Storage::createDir(SD_RECON_DIR)) {
        return false;
    }

    char basename[40];
    snprintf(basename, sizeof(basename), "recon_%lu.pcap", (unsigned long)millis());
    char fullPath[64];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", SD_RECON_DIR, basename);
    if (!PcapWriter::open(fullPath)) {
        return false;
    }
    strncpy(_currentFile, basename, sizeof(_currentFile) - 1);
    _currentFile[sizeof(_currentFile) - 1] = '\0';

    _head = 0;
    _tail = 0;
    _dropped = 0;
    _packetCount = 0;

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    esp_wifi_set_promiscuous(true);

    _capturing = true;
    if (outPath && outPathLen > 0) {
        strncpy(outPath, _currentFile, outPathLen - 1);
        outPath[outPathLen - 1] = '\0';
    }
    Serial.printf("[RECON] Capture started -> %s\n", _currentFile);
    return true;
}

void Recon::stopCapture()
{
    if (!_capturing) return;
    esp_wifi_set_promiscuous(false);
    tick(); // drain whatever's left in the ring before closing the file
    PcapWriter::close();
    _capturing = false;
    Serial.printf("[RECON] Capture stopped: %lu packets, %lu dropped\n",
                  (unsigned long)_packetCount, (unsigned long)_dropped);
}

bool Recon::capturing() { return _capturing; }

// Derive band from primary channel: 1..14 = 2.4 GHz, 32+ = 5 GHz (U-NII).
static bool channelIs5G(uint8_t ch) { return ch >= 32; }

// WPA3 (and WPA2/WPA3-transition) implies PMF. This is the 2a best-effort
// heuristic; Phase 2b reads MFPC/MFPR from the beacon RSN IE for certainty.
static bool authImpliesPmf(uint8_t authmode)
{
    return authmode == WIFI_AUTH_WPA3_PSK ||
           authmode == WIFI_AUTH_WPA2_WPA3_PSK ||
           authmode == WIFI_AUTH_WPA3_ENT_192 ||
           authmode == WIFI_AUTH_WPA3_EXT_PSK;
}

// Pull the finished async scan into the RAM table, print it to serial (the
// reliable readout), free the driver's copy, and do a light AP recovery.
// Called from tick() once the async scan reports done.
static void harvestScan()
{
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;   // still sweeping

    _apCount = 0;
    if (n > 0) {
        uint32_t cap = (uint32_t)n < CFG_RECON_SCAN_MAX_APS ? (uint32_t)n
                                                            : CFG_RECON_SCAN_MAX_APS;
        for (uint32_t i = 0; i < cap; i++) {
            Recon::ApRecord& r = _aps[_apCount];
            const uint8_t* b = WiFi.BSSID(i);
            if (b) memcpy(r.bssid, b, 6); else memset(r.bssid, 0, 6);
            String ssid = WiFi.SSID(i);
            r.hidden = (ssid.length() == 0);
            strncpy(r.ssid, ssid.c_str(), sizeof(r.ssid) - 1);
            r.ssid[sizeof(r.ssid) - 1] = '\0';
            r.channel = (uint8_t)WiFi.channel(i);
            r.band5 = channelIs5G(r.channel);
            r.rssi = (int8_t)WiFi.RSSI(i);
            r.authmode = (uint8_t)WiFi.encryptionType(i);
            r.pmf = authImpliesPmf(r.authmode);
            _apCount++;
        }
    }
    WiFi.scanDelete();   // release the driver-side result buffer
    _lastScanMs = millis();
    _scanning = false;

    // Print the AP table FIRST — serial is the authoritative, reliable readout
    // on this hardware (SoftAP TX is unreliable during/after a scan, same
    // reason pcap retrieval uses the serial DUMP path), so it must not depend
    // on any recovery step that follows.
    Serial.printf("[RECON] Scan done: %lu AP(s)%s\n",
                  (unsigned long)_apCount,
                  n < 0 ? " (scan failed)" : "");
    for (uint32_t i = 0; i < _apCount; i++) {
        const Recon::ApRecord& r = _aps[i];
        Serial.printf("[RECON]  %02X:%02X:%02X:%02X:%02X:%02X  ch%-3u %s  %4d dBm  auth=%u%s  %s\n",
                      r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5],
                      r.channel, r.band5 ? "5G  " : "2.4G", r.rssi, r.authmode,
                      r.pmf ? " PMF" : "", r.hidden ? "<hidden>" : r.ssid);
    }

    // Best-effort recovery of the SoftAP after the APSTA sweep. Drop ONLY the
    // STA interface the scan added (return to AP-only) — a light, non-blocking
    // mode change. A full SoftAP re-init (WiFi.softAP) from this scan-complete
    // path HUNG the main loop (confirmed on hardware 2026-09-06), so it is
    // deliberately NOT used. REST-over-SoftAP is best-effort on this hardware;
    // the serial table above is the readout that matters. Markers bracket the
    // call so any stall here is pinpointed on serial.
    Serial.println("[RECON] light AP recovery (drop STA)...");
    WiFi.enableSTA(false);
    Serial.println("[RECON] AP recovery done");
}

void Recon::tick()
{
    if (_scanning) harvestScan();

    while (_tail != _head) {
        ReconFrame& slot = _ring[_tail];
        PcapWriter::writeFrame(slot.data, slot.len, slot.origLen,
                                slot.timestampUs, slot.rssi, slot.channel);
        _packetCount++;
        // EAPOL frames are the interesting, rare event (4 per handshake) —
        // worth a line each. Beacons are not: at ~10/s they'd flood the
        // console, so they only show up in the periodic heartbeat below.
        // This is the serial-side cross-check for a capture in progress,
        // independent of the REST /api/recon/status packet/dropped counts.
        if (slot.isEapol) {
            Serial.printf("[RECON] EAPOL frame captured (%u bytes, rssi %d dBm)\n",
                          slot.len, slot.rssi);
        } else if (_packetCount % 20 == 0) {
            Serial.printf("[RECON] %lu packets captured so far (%lu dropped)\n",
                          (unsigned long)_packetCount, (unsigned long)_dropped);
        }
        _tail = (_tail + 1) % CFG_RECON_RING_SLOTS;
    }
}

uint32_t Recon::packetCount() { return _packetCount; }
uint32_t Recon::droppedCount() { return _dropped; }
const char* Recon::currentFile() { return _currentFile; }

// ── Phase 2a: dual-band AP scan ─────────────────────────────────────────────
bool Recon::startScan(bool keepApUp)
{
    if (_capturing || _scanning) return false;   // one radio — no overlap

    // The SoftAP-DOWN variant is deferred: tearing the AP down for the sweep
    // required a full SoftAP re-init to restore it, which hung the main loop
    // from the scan-complete path (2026-09-06). The AP-UP scan already
    // enumerates BOTH bands (5 GHz APs appear in the results), so every scan
    // runs AP-up for now regardless of keepApUp; AP-down returns once there's
    // a safe AP-restore path.
    (void)keepApUp;

    // async=true, show_hidden=true, passive=false, dwell per channel, ch 0=all.
    // Arduino brings up STA for the scan; with the AP up this is APSTA and the
    // dashboard keeps serving between channel hops.
    int16_t rc = WiFi.scanNetworks(true, true, false, CFG_RECON_SCAN_DWELL_MS, 0);
    if (rc != WIFI_SCAN_RUNNING) {
        Serial.printf("[RECON] Scan start failed (rc=%d)\n", rc);
        return false;
    }

    _scanning = true;
    _apCount = 0;
    Serial.println("[RECON] Scan started (SoftAP up)");
    return true;
}

bool Recon::scanning() { return _scanning; }
uint32_t Recon::apCount() { return _apCount; }
const Recon::ApRecord* Recon::apRecord(uint32_t i) { return i < _apCount ? &_aps[i] : nullptr; }
uint32_t Recon::lastScanMillis() { return _lastScanMs; }
