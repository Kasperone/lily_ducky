// =============================================================================
// recon/pcap_writer.h — libpcap (radiotap/802.11 linktype) file writer
// =============================================================================
// Standard libpcap format so captures open directly in Wireshark. No custom
// encoding — see docs/knowledge-base or the Module B design discussion for
// why .hc22000 conversion is deliberately NOT done on-device (hcxpcapngtool
// on the downloaded PCAP, off-device, is the intended path).
// =============================================================================
#ifndef LILY_DUCKY_PCAP_WRITER_H
#define LILY_DUCKY_PCAP_WRITER_H

#include <Arduino.h>

namespace PcapWriter {

    // Opens `path` on the SD card and writes the libpcap global header.
    // Truncates/overwrites if the file already exists.
    bool open(const char* path);

    // Appends one captured frame: per-packet header + a minimal radiotap
    // header (channel + RSSI) + the raw 802.11 bytes, truncated to
    // CFG_RECON_SNAPLEN. `origLen` is the frame's true on-air length
    // (rx_ctrl.sig_len) even when `data`/`len` were already truncated by the
    // ring buffer.
    bool writeFrame(const uint8_t* data, uint16_t len, uint16_t origLen,
                     uint32_t timestampUs, int8_t rssi, uint8_t channel);

    void close();
    bool isOpen();

} // namespace PcapWriter

#endif
