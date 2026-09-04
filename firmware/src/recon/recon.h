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

} // namespace Recon

#endif
