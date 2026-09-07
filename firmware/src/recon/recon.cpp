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
    bool     isEapol;      // CAPTURE mode only: true for WIFI_PKT_DATA frames
                            // (EAPOL by construction — see promiscuousCb)
};

static ReconFrame _ring[CFG_RECON_RING_SLOTS];
static volatile uint16_t _head = 0;
static volatile uint16_t _tail = 0;
static volatile uint32_t _dropped = 0;
static uint32_t _packetCount = 0;
static char _currentFile[40] = "";  // basename only (matches the payload API's convention)
static bool _capturing = false;

// Which consumer tick() feeds drained ring frames to, and which classifier
// promiscuousCb applies to accept a frame into the ring in the first place.
// Only one of these (or plain capture) ever owns the radio at a time — see
// radioBusy() — so the ring buffer and its producer/consumer pair are safely
// shared across all of them rather than duplicated per mode.
enum class ReconMode : uint8_t { NONE, CAPTURE, PMF_SWEEP, STA_ENUM };
static ReconMode _ringMode = ReconMode::NONE;

// ── Phase 2a scan state ─────────────────────────────────────────────────────
// The managed scan (WiFi.scanNetworks) is async; tick() harvests the result
// once WiFi.scanComplete() reports done. Scan and capture are mutually
// exclusive (one radio) — both start functions refuse if the other is active.
static Recon::ApRecord _aps[CFG_RECON_SCAN_MAX_APS];
static uint32_t _apCount = 0;
static bool _scanning = false;
static uint32_t _lastScanMs = 0;

// ── Phase 2b: RSN-IE PMF sweep state ────────────────────────────────────────
// Runs automatically right after a managed scan completes (see harvestScan),
// never concurrently with one — it hops channels itself via
// esp_wifi_set_channel(), deliberately not stacked on scanNetworks()'s own
// async hop (untested combination; see config.h).
static bool _pmfSweeping = false;
static uint8_t _pmfChannels[CFG_RECON_SCAN_MAX_APS]; // unique channels from the AP table just scanned
static uint8_t _pmfChannelCount = 0;
static uint8_t _pmfChannelIdx = 0;
static uint32_t _pmfDwellStartMs = 0;
static uint32_t _pmfConfirmed = 0; // APs whose pmfStatus left UNKNOWN this sweep

// ── Phase 2b: station enumeration state ─────────────────────────────────────
// Manual, one AP at a time (Recon::startEnum). Same explicit-channel
// promiscuous approach as the PMF sweep, reusing the same ring buffer.
static Recon::StaRecord _stas[CFG_RECON_MAX_STAS];
static uint32_t _staCount = 0;
static bool _enumRunning = false;
static uint8_t _enumTargetBssid[6];
static uint32_t _enumStartMs = 0;

// True while any mode already owns the single radio — every start* function
// must check this before touching WiFi/esp_wifi state.
static bool radioBusy()
{
    return _capturing || _scanning || _pmfSweeping || _enumRunning;
}

bool Recon::busy() { return radioBusy(); }

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

// Management frame, Beacon(8) or Probe-Response(5) subtype — the only two
// frame types that carry an RSN IE we can read PMF out of.
static bool isPmfSweepFrame(wifi_promiscuous_pkt_type_t type, const uint8_t* f, uint16_t len)
{
    if (type != WIFI_PKT_MGMT || len < 1) return false;
    uint8_t fc0 = f[0];
    uint8_t mtype = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0xF;
    return mtype == 0 && (subtype == 8 || subtype == 5);
}

// Resolves the non-BSSID address of a Data frame via its ToDS/FromDS bits,
// iff the frame's other address matches `target`. Returns false for WDS
// (+Addr4, both bits set) and IBSS/no-DS frames (neither bit set) — the C5's
// own AP/station lab traffic uses neither, so this is a known, documented
// gap (same convention as isEapolDataFrame above) rather than silently
// mishandled.
static bool resolveStationMac(const uint8_t* f, uint16_t len, const uint8_t target[6], uint8_t staMacOut[6])
{
    if (len < 24) return false;
    uint8_t fc1 = f[1];
    bool toDS = fc1 & 0x1;
    bool fromDS = (fc1 >> 1) & 0x1;
    if (toDS == fromDS) return false; // WDS or IBSS/no-DS — not this lab's topology

    const uint8_t* addr1 = f + 4;
    const uint8_t* addr2 = f + 10;
    const uint8_t* bssid = fromDS ? addr2 : addr1;
    const uint8_t* station = fromDS ? addr1 : addr2;
    if (memcmp(bssid, target, 6) != 0) return false;
    memcpy(staMacOut, station, 6);
    return true;
}

// Runs in the Wi-Fi driver task (see recon.h) — kept to cheap branches plus
// one bounded memcpy. No Serial, no SD, no malloc: all deferred to tick().
// Which frames are accepted into the ring depends on _ringMode — capture,
// PMF sweep and station enum each want a different slice of the air.
static void promiscuousCb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (_ringMode == ReconMode::NONE) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;

    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* frame = pkt->payload;
    uint16_t origLen = pkt->rx_ctrl.sig_len;

    bool accept = false;
    switch (_ringMode) {
        case ReconMode::CAPTURE:
            // Data frames: only keep ones that decode as EAPOL, so ordinary
            // client traffic (the Win11 VM's normal browsing, C2 dashboard
            // HTTP, etc.) never enters the ring. Management frames (beacon/
            // probe/auth/assoc/deauth) are all kept.
            accept = (type == WIFI_PKT_MGMT) || isEapolDataFrame(frame, origLen);
            break;
        case ReconMode::PMF_SWEEP:
            accept = isPmfSweepFrame(type, frame, origLen);
            break;
        case ReconMode::STA_ENUM: {
            if (type != WIFI_PKT_DATA) break;
            uint8_t staMac[6];
            accept = resolveStationMac(frame, origLen, _enumTargetBssid, staMac);
            break;
        }
        default: break;
    }
    if (!accept) return;

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
    // Only CAPTURE mode's tick() drain consults isEapol; only WIFI_PKT_DATA
    // frames reach here in that mode and they're EAPOL by construction.
    slot.isEapol = (_ringMode == ReconMode::CAPTURE) && (type == WIFI_PKT_DATA);
    _head = next;
}

bool Recon::startCapture(char* outPath, size_t outPathLen)
{
    if (radioBusy()) return false;   // one radio — no overlap
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

    _ringMode = ReconMode::CAPTURE;
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
    _ringMode = ReconMode::NONE;
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

static const char* pmfStatusStr(Recon::PmfStatus s)
{
    switch (s) {
        case Recon::PmfStatus::NOT_PROTECTED: return "disabled";
        case Recon::PmfStatus::CAPABLE:  return "capable";
        case Recon::PmfStatus::REQUIRED: return "required";
        default:                         return "unknown";
    }
}

// Parses the RSN IE (tag 0x30) out of a Beacon/Probe-Response management
// frame to get PMF's real MFPC/MFPR bits (802.11-2020 Figure 9-262: bit 6 =
// MFPR, bit 7 = MFPC in the 2-byte RSN Capabilities field). The managed-scan
// API's wifi_ap_record_t has no such field — confirmed against this
// toolchain's esp_wifi_types_generic.h before writing this — so this raw-
// frame walk is the only way to get a real answer rather than 2a's
// authmode-implies-PMF guess. Every step bounds-checks against the frame/IE
// end before reading: these are raw over-the-air bytes, not something to
// trust blindly even on a lab AP. `f` must already be known to be a
// Beacon/Probe-Response (see isPmfSweepFrame) — this only interprets it.
static bool parseBeaconRsnPmf(const uint8_t* f, uint16_t len, uint8_t bssidOut[6], Recon::PmfStatus& pmfOut)
{
    if (len < 36) return false; // no room for the 24B header + 12B fixed fields
    memcpy(bssidOut, f + 10, 6); // Address 2 (SA) == BSSID for AP-transmitted frames

    uint32_t pos = 36;
    while (pos + 2 <= len) {
        uint8_t id = f[pos];
        uint8_t ieLen = f[pos + 1];
        uint32_t ieStart = pos + 2;
        if (ieStart + ieLen > len) break; // truncated IE (snaplen or malformed) — stop, don't guess

        if (id == 0x30) { // RSN
            uint32_t p = ieStart;
            uint32_t end = ieStart + ieLen;
            if (p + 2 > end) return false; p += 2;  // version
            if (p + 4 > end) return false; p += 4;  // group cipher suite
            if (p + 2 > end) return false;
            uint32_t pairwiseCount = f[p] | ((uint32_t)f[p + 1] << 8); p += 2;
            uint32_t pairwiseBytes = pairwiseCount * 4;
            if (p + pairwiseBytes > end) return false; p += pairwiseBytes;
            if (p + 2 > end) return false;
            uint32_t akmCount = f[p] | ((uint32_t)f[p + 1] << 8); p += 2;
            uint32_t akmBytes = akmCount * 4;
            if (p + akmBytes > end) return false; p += akmBytes;

            if (p + 2 > end) {
                // RSN Capabilities is technically optional (defaults to
                // all-zero, i.e. PMF not advertised) — treat an IE that
                // truncates exactly here the same way rather than guessing.
                pmfOut = Recon::PmfStatus::NOT_PROTECTED;
                return true;
            }
            uint16_t rsnCaps = f[p] | ((uint16_t)f[p + 1] << 8);
            bool mfpr = (rsnCaps >> 6) & 0x1;
            bool mfpc = (rsnCaps >> 7) & 0x1;
            pmfOut = mfpr ? Recon::PmfStatus::REQUIRED
                   : mfpc ? Recon::PmfStatus::CAPABLE
                          : Recon::PmfStatus::NOT_PROTECTED;
            return true;
        }
        pos = ieStart + ieLen;
    }
    return false; // no RSN IE in this frame (open network or WPA1-only)
}

// Unlike WiFi.scanNetworks(), our own esp_wifi_set_channel() calls don't
// self-restore the AP's operating channel afterward — do it explicitly, then
// apply the same light STA-drop recovery harvestScan() already uses. This
// was new territory vs. 2a's scan-complete path (which only ever called
// WiFi.enableSTA(false), never esp_wifi_set_channel()) — hardware-confirmed
// safe 2026-09-07 via the explicit PMF/ENUM commands (no loop hang across
// repeated channel-hop-then-recovery cycles; see AGENTS.md's Module B note).
static void restoreApChannelAndRecover()
{
    uint8_t apChannel =
#if CFG_WIFI_BAND_5G
        CFG_WIFI_5G_CHANNEL;
#else
        CFG_WIFI_CHANNEL;
#endif
    esp_wifi_set_channel(apChannel, WIFI_SECOND_CHAN_NONE);
    Serial.println("[RECON] light AP recovery (drop STA, restore channel)...");
    WiFi.enableSTA(false);
    Serial.println("[RECON] AP recovery done");
}

// ── Phase 2b: RSN-IE PMF sweep ──────────────────────────────────────────────
// Builds the unique-channel list from the AP table a scan just produced and
// starts hopping it. Caller (either the auto-chain in harvestScan(), gated
// by CFG_RECON_AUTO_PMF_SWEEP, or the explicit Recon::startPmfSweep()) is
// responsible for the radioBusy() check — this just does the mechanics.
// Returns false if there's nothing to sweep (empty AP table).
static bool startPmfSweepInternal()
{
    _pmfChannelCount = 0;
    for (uint32_t i = 0; i < _apCount && _pmfChannelCount < CFG_RECON_SCAN_MAX_APS; i++) {
        uint8_t ch = _aps[i].channel;
        bool seen = false;
        for (uint8_t j = 0; j < _pmfChannelCount; j++) {
            if (_pmfChannels[j] == ch) { seen = true; break; }
        }
        if (!seen) _pmfChannels[_pmfChannelCount++] = ch;
    }
    if (_pmfChannelCount == 0) return false; // nothing found by the scan — nothing to sweep

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    esp_wifi_set_promiscuous(true);

    _ringMode = ReconMode::PMF_SWEEP;
    _pmfChannelIdx = 0;
    _pmfDwellStartMs = 0;
    _pmfConfirmed = 0;
    _pmfSweeping = true;
    Serial.printf("[RECON] PMF sweep starting: %u channel(s)\n", (unsigned)_pmfChannelCount);
    return true;
}

static void finishPmfSweep()
{
    esp_wifi_set_promiscuous(false);
    _ringMode = ReconMode::NONE;
    _pmfSweeping = false;
    restoreApChannelAndRecover();

    Serial.printf("[RECON] PMF sweep done: %lu/%lu AP(s) confirmed\n",
                  (unsigned long)_pmfConfirmed, (unsigned long)_apCount);
    for (uint32_t i = 0; i < _apCount; i++) {
        const Recon::ApRecord& r = _aps[i];
        if (r.pmfStatus == Recon::PmfStatus::UNKNOWN) continue; // no beacon seen this sweep
        Serial.printf("[RECON]  %02X:%02X:%02X:%02X:%02X:%02X  pmf=%s\n",
                      r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5],
                      pmfStatusStr(r.pmfStatus));
    }
}

// One tick's worth of the sweep: begin a channel's dwell, or advance past it
// once CFG_RECON_PMF_DWELL_MS has elapsed. `_pmfDwellStartMs == 0` means "not
// yet begun for the current channel" — millis() is never actually 0 by the
// time a sweep can run (well past boot), so this is a safe sentinel.
static void tickPmfSweep()
{
    if (_pmfChannelIdx >= _pmfChannelCount) { finishPmfSweep(); return; }

    if (_pmfDwellStartMs == 0) {
        esp_wifi_set_channel(_pmfChannels[_pmfChannelIdx], WIFI_SECOND_CHAN_NONE);
        _pmfDwellStartMs = millis();
        return;
    }
    if (millis() - _pmfDwellStartMs >= CFG_RECON_PMF_DWELL_MS) {
        _pmfChannelIdx++;
        _pmfDwellStartMs = 0;
    }
}

// ── Phase 2b: station enumeration ───────────────────────────────────────────
static void upsertStation(const uint8_t mac[6], int8_t rssi)
{
    for (uint32_t i = 0; i < _staCount; i++) {
        if (memcmp(_stas[i].mac, mac, 6) == 0) {
            _stas[i].rssi = rssi;
            _stas[i].lastSeenMs = millis();
            return;
        }
    }
    if (_staCount >= CFG_RECON_MAX_STAS) return; // table full — drop, don't block
    Recon::StaRecord& s = _stas[_staCount];
    memcpy(s.mac, mac, 6);
    s.rssi = rssi;
    s.lastSeenMs = millis();
    _staCount++;
    Serial.printf("[RECON-STA] %02X:%02X:%02X:%02X:%02X:%02X  rssi=%d dBm\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
}

static void finishEnum()
{
    esp_wifi_set_promiscuous(false);
    _ringMode = ReconMode::NONE;
    _enumRunning = false;
    restoreApChannelAndRecover();
    Serial.printf("[RECON-STA] done: %lu station(s)\n", (unsigned long)_staCount);
}

static void tickEnum()
{
    if (millis() - _enumStartMs >= CFG_RECON_ENUM_DWELL_MS) finishEnum();
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
            r.pmfStatus = Recon::PmfStatus::UNKNOWN; // confirmed (or not) by the sweep below
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
        // pmf= here is still the 2a authmode heuristic (heuristic) — the PMF
        // sweep chained right after this replaces it with confirmed values
        // read from the actual RSN IE (see finishPmfSweep).
        Serial.printf("[RECON]  %02X:%02X:%02X:%02X:%02X:%02X  ch%-3u %s  %4d dBm  auth=%u  pmf=%s(heuristic)  %s\n",
                      r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4], r.bssid[5],
                      r.channel, r.band5 ? "5G  " : "2.4G", r.rssi, r.authmode,
                      r.pmf ? "capable" : "disabled", r.hidden ? "<hidden>" : r.ssid);
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

    // Phase 2b: automatically confirm PMF for whatever the scan just found,
    // sequentially — never concurrently with the managed scan itself (see
    // config.h and startPmfSweepInternal). Gated OFF by default (see
    // CFG_RECON_AUTO_PMF_SWEEP in config.h): the sweep's channel-hop-then-
    // recovery path is now hardware-confirmed safe via the explicit `PMF`
    // command (2026-09-07, no loop hang — see AGENTS.md), but the flag stays
    // off pending a decision on whether to fold it back into plain SCAN.
#if CFG_RECON_AUTO_PMF_SWEEP
    startPmfSweepInternal();
#endif
}

void Recon::tick()
{
    if (_scanning) harvestScan();

    // Drain whatever's already in the ring under the mode that produced it
    // BEFORE advancing the PMF-sweep/enum state machines below — those can
    // flip _ringMode (e.g. on the sweep's last channel finishing), which
    // would otherwise strand any frames still queued from that channel.
    while (_tail != _head) {
        ReconFrame& slot = _ring[_tail];
        switch (_ringMode) {
            case ReconMode::CAPTURE:
                PcapWriter::writeFrame(slot.data, slot.len, slot.origLen,
                                        slot.timestampUs, slot.rssi, slot.channel);
                _packetCount++;
                // EAPOL frames are the interesting, rare event (4 per
                // handshake) — worth a line each. Beacons are not: at ~10/s
                // they'd flood the console, so they only show up in the
                // periodic heartbeat below. This is the serial-side cross-
                // check for a capture in progress, independent of the REST
                // /api/recon/status packet/dropped counts.
                if (slot.isEapol) {
                    Serial.printf("[RECON] EAPOL frame captured (%u bytes, rssi %d dBm)\n",
                                  slot.len, slot.rssi);
                } else if (_packetCount % 20 == 0) {
                    Serial.printf("[RECON] %lu packets captured so far (%lu dropped)\n",
                                  (unsigned long)_packetCount, (unsigned long)_dropped);
                }
                break;
            case ReconMode::PMF_SWEEP: {
                uint8_t bssid[6];
                Recon::PmfStatus pmf;
                if (parseBeaconRsnPmf(slot.data, slot.len, bssid, pmf)) {
                    for (uint32_t i = 0; i < _apCount; i++) {
                        if (memcmp(_aps[i].bssid, bssid, 6) == 0) {
                            if (_aps[i].pmfStatus == Recon::PmfStatus::UNKNOWN) _pmfConfirmed++;
                            _aps[i].pmfStatus = pmf;
                            break;
                        }
                    }
                }
                break;
            }
            case ReconMode::STA_ENUM: {
                uint8_t staMac[6];
                if (resolveStationMac(slot.data, slot.len, _enumTargetBssid, staMac)) {
                    upsertStation(staMac, slot.rssi);
                }
                break;
            }
            default: break;
        }
        _tail = (_tail + 1) % CFG_RECON_RING_SLOTS;
    }

    if (_pmfSweeping) tickPmfSweep();
    if (_enumRunning) tickEnum();
}

uint32_t Recon::packetCount() { return _packetCount; }
uint32_t Recon::droppedCount() { return _dropped; }
const char* Recon::currentFile() { return _currentFile; }

// ── Phase 2a: dual-band AP scan ─────────────────────────────────────────────
bool Recon::startScan(bool keepApUp)
{
    if (radioBusy()) return false;   // one radio — no overlap

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

bool Recon::startPmfSweep()
{
    if (radioBusy()) return false;
    return startPmfSweepInternal();
}

bool Recon::pmfSweeping() { return _pmfSweeping; }

// ── Phase 2b: station enumeration (public entry point) ─────────────────────
bool Recon::startEnum(uint32_t apIndex)
{
    if (radioBusy()) return false;
    if (apIndex >= _apCount) return false;

    const Recon::ApRecord& target = _aps[apIndex];
    memcpy(_enumTargetBssid, target.bssid, 6);
    esp_wifi_set_channel(target.channel, WIFI_SECOND_CHAN_NONE);

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuousCb);
    esp_wifi_set_promiscuous(true);

    _ringMode = ReconMode::STA_ENUM;
    _staCount = 0;
    _enumStartMs = millis();
    _enumRunning = true;

    Serial.printf("[RECON-STA] begin bssid=%02X:%02X:%02X:%02X:%02X:%02X channel=%u\n",
                  target.bssid[0], target.bssid[1], target.bssid[2],
                  target.bssid[3], target.bssid[4], target.bssid[5], target.channel);
    return true;
}

bool Recon::enumRunning() { return _enumRunning; }
uint32_t Recon::staCount() { return _staCount; }
const Recon::StaRecord* Recon::staRecord(uint32_t i) { return i < _staCount ? &_stas[i] : nullptr; }
