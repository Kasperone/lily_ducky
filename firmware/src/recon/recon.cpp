// =============================================================================
// recon/recon.cpp — WiFi 802.11 promiscuous capture -> PCAP on SD (Module B)
// =============================================================================
#include "recon.h"
#include <esp_wifi.h>
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
    if (_capturing) return false;
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

void Recon::tick()
{
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
