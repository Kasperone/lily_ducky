// =============================================================================
// pcap_writer.h - libpcap (radiotap/802.11) file writer on plain ESP-IDF
// =============================================================================
// Port of lily_ducky firmware/src/recon/pcap_writer.cpp with the Arduino File
// object replaced by stdio/FILE on the VFS-mounted FAT card.
//
// Standard libpcap format so captures open directly in Wireshark: 24-byte
// global header (LINKTYPE_IEEE802_11_RADIOTAP = 127), then one record per
// frame = 16-byte record header + a minimal 15-byte radiotap header (channel +
// RSSI) + the raw 802.11 bytes. No custom encoding; .hc22000 conversion is
// deliberately done off-device (hcxpcapngtool on the pulled file).
//
// Only one capture file is open at a time (there is one radio), so this keeps
// a single static FILE* rather than a handle. Not thread-safe on its own: the
// caller (recon.c) serialises it from its tick task, and no other task touches
// captures.
// =============================================================================
#ifndef C5_PCAP_WRITER_H
#define C5_PCAP_WRITER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens `path` (absolute, e.g. "/sdcard/recon/recon_1234.pcap") and writes the
// libpcap global header. Truncates an existing file. False on any I/O error.
bool pcap_open(const char *path);

// Appends one captured frame: record header + radiotap + `len` bytes of
// 802.11. `origLen` is the frame's true on-air length (rx_ctrl.sig_len), which
// may exceed `len` when the ring buffer already truncated to the snaplen.
// `len` is clamped to CFG_RECON_SNAPLEN. Each frame is flushed, matching lily:
// this is a lab capture where losing the handshake to a crash matters more
// than write throughput (the MGMT/EAPOL filter keeps the rate low).
bool pcap_write_frame(const uint8_t *data, uint16_t len, uint16_t origLen,
                      uint32_t timestampUs, int8_t rssi, uint8_t channel);

// Flushes and closes. Safe when nothing is open.
void pcap_close(void);

bool pcap_is_open(void);

// Bytes actually written to the current file (0 when closed). Used by the LCD
// and the console to cross-check that a capture is producing data.
uint32_t pcap_bytes_written(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_PCAP_WRITER_H
