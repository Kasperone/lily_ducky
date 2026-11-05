// =============================================================================
// pcap_writer.c - libpcap (radiotap/802.11) file writer (see pcap_writer.h)
// =============================================================================
#include "pcap_writer.h"

#include <stdio.h>

#include "recon_config.h"

// LINKTYPE_IEEE802_11_RADIOTAP - tcpdump.org/linktypes.html #127. Wireshark
// and tcpdump both dissect this directly as 802.11 framed by a radiotap
// pseudo-header; no vendor-specific linktype needed.
#define LINKTYPE_IEEE802_11_RADIOTAP 127

// Minimal radiotap header: Flags + Channel + dBm Antenna Signal only. Bit
// numbers verified against the Linux kernel's ieee80211_radiotap.h
// (FLAGS=1, CHANNEL=3, DBM_ANTSIGNAL=5) - an earlier, wrong secondary source
// claimed DBM_ANTSIGNAL=9; do not trust that number if it resurfaces.
//
// Byte layout (offsets from the start of the radiotap header):
//   0-1  it_version/it_pad      (0, 0)
//   2-3  it_len                 (15, little-endian)
//   4-7  it_present             (FLAGS|CHANNEL|DBM_ANTSIGNAL = 0x2A, LE)
//   8    Flags                  (1 byte, none set)
//   9    pad                    (aligns the 2-byte Channel field to %2==0)
//   10-11 Channel: freq MHz     (little-endian)
//   12-13 Channel: flags        (little-endian; 0x0080 = IEEE80211_CHAN_2GHZ)
//   14   dBm Antenna Signal     (1 byte, signed)
#define RADIOTAP_LEN 15

static FILE *s_f;
static uint32_t s_bytes;

// Channel -> centre frequency (MHz). 2.4 GHz ISM mapping; channel 14
// (Japan-only, 2484 MHz) is the one exception to the *5 rule. 5 GHz channels
// use the U-NII rule, which is 5000 + ch*5 across all three U-NII groups
// (36..64, 100..144, 149..177) - it is genuinely the same formula, not three
// cases.
static uint16_t channel_to_freq_mhz(uint8_t channel)
{
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) return (uint16_t)(2407 + (channel * 5));
    if (channel >= 32 && channel <= 177) return (uint16_t)(5000 + channel * 5);
    return 2412;  // unknown input - fall back to channel 1 rather than emit garbage
}

static void build_radiotap(uint8_t *out, int8_t rssi, uint8_t channel)
{
    out[0] = 0;                 // it_version
    out[1] = 0;                 // it_pad
    out[2] = RADIOTAP_LEN;      // it_len (LE)
    out[3] = 0;
    out[4] = 0x2A;              // it_present: FLAGS(1<<1)|CHANNEL(1<<3)|DBM_ANTSIGNAL(1<<5)
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    out[8] = 0;                 // Flags: none set
    out[9] = 0;                 // pad (align Channel to even offset)
    uint16_t freq = channel_to_freq_mhz(channel);
    out[10] = (uint8_t)(freq & 0xFF);
    out[11] = (uint8_t)((freq >> 8) & 0xFF);
    // Channel flags LE. 0x0080 = IEEE80211_CHAN_2GHZ, 0x0100 = _5GHZ; a 5 GHz
    // record tagged 2 GHZ still dissects, but Wireshark shows the wrong band.
    bool is5g = (channel >= 32);
    out[12] = is5g ? 0x00 : 0x80;
    out[13] = is5g ? 0x01 : 0x00;
    out[14] = (uint8_t)rssi;    // dBm Antenna Signal (signed, as a raw byte)
}

bool pcap_open(const char *path)
{
    if (!path) return false;
    if (s_f) pcap_close();

    s_f = fopen(path, "wb");
    if (!s_f) return false;
    s_bytes = 0;

    // libpcap global header, microsecond resolution, native (LE) byte order.
    uint8_t hdr[24];
    hdr[0] = 0xd4; hdr[1] = 0xc3; hdr[2] = 0xb2; hdr[3] = 0xa1;  // magic 0xa1b2c3d4 LE
    hdr[4] = 2; hdr[5] = 0;                                       // version_major = 2
    hdr[6] = 4; hdr[7] = 0;                                       // version_minor = 4
    hdr[8] = 0; hdr[9] = 0; hdr[10] = 0; hdr[11] = 0;             // thiszone = 0
    hdr[12] = 0; hdr[13] = 0; hdr[14] = 0; hdr[15] = 0;           // sigfigs = 0
    uint32_t snaplen = RADIOTAP_LEN + CFG_RECON_SNAPLEN;
    hdr[16] = (uint8_t)(snaplen & 0xFF);
    hdr[17] = (uint8_t)((snaplen >> 8) & 0xFF);
    hdr[18] = (uint8_t)((snaplen >> 16) & 0xFF);
    hdr[19] = (uint8_t)((snaplen >> 24) & 0xFF);
    hdr[20] = (uint8_t)(LINKTYPE_IEEE802_11_RADIOTAP & 0xFF);
    hdr[21] = (uint8_t)((LINKTYPE_IEEE802_11_RADIOTAP >> 8) & 0xFF);
    hdr[22] = (uint8_t)((LINKTYPE_IEEE802_11_RADIOTAP >> 16) & 0xFF);
    hdr[23] = (uint8_t)((LINKTYPE_IEEE802_11_RADIOTAP >> 24) & 0xFF);

    if (fwrite(hdr, 1, sizeof(hdr), s_f) != sizeof(hdr)) {
        fclose(s_f);
        s_f = NULL;
        return false;
    }
    fflush(s_f);
    s_bytes = sizeof(hdr);
    return true;
}

bool pcap_write_frame(const uint8_t *data, uint16_t len, uint16_t origLen,
                      uint32_t timestampUs, int8_t rssi, uint8_t channel)
{
    if (!s_f) return false;
    if (!data) return false;
    if (len > CFG_RECON_SNAPLEN) len = CFG_RECON_SNAPLEN;  // ring already guarantees this

    uint8_t radiotap[RADIOTAP_LEN];
    build_radiotap(radiotap, rssi, channel);

    uint32_t inclLen = RADIOTAP_LEN + len;
    uint32_t origTotal = RADIOTAP_LEN + origLen;

    uint8_t pktHdr[16];
    uint32_t tsSec = timestampUs / 1000000UL;
    uint32_t tsUsec = timestampUs % 1000000UL;
    pktHdr[0] = (uint8_t)(tsSec & 0xFF);
    pktHdr[1] = (uint8_t)((tsSec >> 8) & 0xFF);
    pktHdr[2] = (uint8_t)((tsSec >> 16) & 0xFF);
    pktHdr[3] = (uint8_t)((tsSec >> 24) & 0xFF);
    pktHdr[4] = (uint8_t)(tsUsec & 0xFF);
    pktHdr[5] = (uint8_t)((tsUsec >> 8) & 0xFF);
    pktHdr[6] = (uint8_t)((tsUsec >> 16) & 0xFF);
    pktHdr[7] = (uint8_t)((tsUsec >> 24) & 0xFF);
    pktHdr[8] = (uint8_t)(inclLen & 0xFF);
    pktHdr[9] = (uint8_t)((inclLen >> 8) & 0xFF);
    pktHdr[10] = (uint8_t)((inclLen >> 16) & 0xFF);
    pktHdr[11] = (uint8_t)((inclLen >> 24) & 0xFF);
    pktHdr[12] = (uint8_t)(origTotal & 0xFF);
    pktHdr[13] = (uint8_t)((origTotal >> 8) & 0xFF);
    pktHdr[14] = (uint8_t)((origTotal >> 16) & 0xFF);
    pktHdr[15] = (uint8_t)((origTotal >> 24) & 0xFF);

    bool ok = fwrite(pktHdr, 1, sizeof(pktHdr), s_f) == sizeof(pktHdr)
           && fwrite(radiotap, 1, RADIOTAP_LEN, s_f) == RADIOTAP_LEN
           && fwrite(data, 1, len, s_f) == len;
    // Flush every frame: this is a lab capture meant to be pulled and analysed
    // later, not a high-rate logger - correctness (don't lose a handshake to a
    // mid-capture crash) matters more than write throughput, and the frame rate
    // after the MGMT/EAPOL filter is low.
    fflush(s_f);
    if (ok) s_bytes += sizeof(pktHdr) + RADIOTAP_LEN + len;
    return ok;
}

void pcap_close(void)
{
    if (!s_f) return;
    fflush(s_f);
    fclose(s_f);
    s_f = NULL;
}

bool pcap_is_open(void) { return s_f != NULL; }

uint32_t pcap_bytes_written(void) { return s_bytes; }
