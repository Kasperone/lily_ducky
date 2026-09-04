// =============================================================================
// recon/pcap_writer.cpp — libpcap (radiotap/802.11 linktype) file writer
// =============================================================================
#include "pcap_writer.h"
#include "config.h"
#include "storage/storage.h"

// LINKTYPE_IEEE802_11_RADIOTAP — tcpdump.org/linktypes.html #127. Wireshark
// and tcpdump both dissect this directly as 802.11 framed by a radiotap
// pseudo-header; no vendor-specific linktype needed.
static const uint32_t LINKTYPE_IEEE802_11_RADIOTAP = 127;

// Minimal radiotap header: Flags + Channel + dBm Antenna Signal only.
// Bit numbers verified against the Linux kernel's ieee80211_radiotap.h
// (FLAGS=1, CHANNEL=3, DBM_ANTSIGNAL=5) — an earlier, wrong secondary
// source claimed DBM_ANTSIGNAL=9; don't trust that number if it resurfaces.
// Byte layout (offsets relative to the start of the radiotap header):
//   0-1  it_version/it_pad      (0, 0)
//   2-3  it_len                 (15, little-endian)
//   4-7  it_present             (FLAGS|CHANNEL|DBM_ANTSIGNAL = 0x2A, LE)
//   8    Flags                  (1 byte)
//   9    pad                    (aligns the 2-byte Channel field to offset%2==0)
//   10-11 Channel: freq MHz     (little-endian)
//   12-13 Channel: flags        (little-endian; 0x0080 = IEEE80211_CHAN_2GHZ,
//                                 radiotap.org Channel-flags field)
//   14   dBm Antenna Signal     (1 byte, signed)
static const uint8_t RADIOTAP_LEN = 15;

static File _f;
static bool _open = false;

// 2.4 GHz channel -> center frequency (MHz). Standard ISM mapping; channel
// 14 (Japan-only, 2484 MHz) is the one exception to the *5 rule. Phase 1
// only ever runs on CFG_WIFI_CHANNEL (2.4 GHz) — 5 GHz center-frequency
// mapping is Phase 2 (scan.cpp), not needed here.
static uint16_t channelToFreqMHz(uint8_t channel)
{
    if (channel == 14) return 2484;
    if (channel >= 1 && channel <= 13) return 2407 + (channel * 5);
    return 2412; // unknown input — fall back to channel 1 rather than emit garbage
}

static void buildRadiotapHeader(uint8_t* out, int8_t rssi, uint8_t channel)
{
    out[0] = 0;                 // it_version
    out[1] = 0;                 // it_pad
    out[2] = RADIOTAP_LEN;      // it_len (LE)
    out[3] = 0;
    out[4] = 0x2A;               // it_present: FLAGS(1<<1) | CHANNEL(1<<3) | DBM_ANTSIGNAL(1<<5)
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    out[8] = 0;                  // Flags: none set
    out[9] = 0;                  // pad (align Channel field to even offset)
    uint16_t freq = channelToFreqMHz(channel);
    out[10] = freq & 0xFF;
    out[11] = (freq >> 8) & 0xFF;
    out[12] = 0x80;              // Channel flags LE: 0x0080 = IEEE80211_CHAN_2GHZ
    out[13] = 0x00;
    out[14] = (uint8_t)rssi;     // dBm Antenna Signal (signed, reinterpreted as raw byte)
}

bool PcapWriter::open(const char* path)
{
    if (_open) close();

    _f = Storage::fs().open(path, FILE_WRITE);
    if (!_f) return false;

    // libpcap global header, microsecond-resolution, native (LE) byte order.
    uint8_t hdr[24];
    hdr[0]=0xd4; hdr[1]=0xc3; hdr[2]=0xb2; hdr[3]=0xa1; // magic (LE 0xa1b2c3d4)
    hdr[4]=2; hdr[5]=0;                                  // version_major = 2
    hdr[6]=4; hdr[7]=0;                                  // version_minor = 4
    hdr[8]=0; hdr[9]=0; hdr[10]=0; hdr[11]=0;            // thiszone = 0
    hdr[12]=0; hdr[13]=0; hdr[14]=0; hdr[15]=0;          // sigfigs = 0
    uint32_t snaplen = RADIOTAP_LEN + CFG_RECON_SNAPLEN;
    hdr[16]=snaplen & 0xFF; hdr[17]=(snaplen>>8)&0xFF;
    hdr[18]=(snaplen>>16)&0xFF; hdr[19]=(snaplen>>24)&0xFF;
    hdr[20]=LINKTYPE_IEEE802_11_RADIOTAP & 0xFF;
    hdr[21]=(LINKTYPE_IEEE802_11_RADIOTAP>>8)&0xFF;
    hdr[22]=(LINKTYPE_IEEE802_11_RADIOTAP>>16)&0xFF;
    hdr[23]=(LINKTYPE_IEEE802_11_RADIOTAP>>24)&0xFF;

    if (_f.write(hdr, sizeof(hdr)) != sizeof(hdr)) {
        _f.close();
        return false;
    }
    _f.flush();
    _open = true;
    return true;
}

bool PcapWriter::writeFrame(const uint8_t* data, uint16_t len, uint16_t origLen,
                             uint32_t timestampUs, int8_t rssi, uint8_t channel)
{
    if (!_open) return false;
    if (len > CFG_RECON_SNAPLEN) len = CFG_RECON_SNAPLEN; // ring buffer should already guarantee this

    uint8_t radiotap[RADIOTAP_LEN];
    buildRadiotapHeader(radiotap, rssi, channel);

    uint32_t inclLen = RADIOTAP_LEN + len;
    uint32_t origTotal = RADIOTAP_LEN + origLen;

    uint8_t pktHdr[16];
    uint32_t tsSec = timestampUs / 1000000UL;
    uint32_t tsUsec = timestampUs % 1000000UL;
    pktHdr[0]=tsSec&0xFF; pktHdr[1]=(tsSec>>8)&0xFF; pktHdr[2]=(tsSec>>16)&0xFF; pktHdr[3]=(tsSec>>24)&0xFF;
    pktHdr[4]=tsUsec&0xFF; pktHdr[5]=(tsUsec>>8)&0xFF; pktHdr[6]=(tsUsec>>16)&0xFF; pktHdr[7]=(tsUsec>>24)&0xFF;
    pktHdr[8]=inclLen&0xFF; pktHdr[9]=(inclLen>>8)&0xFF; pktHdr[10]=(inclLen>>16)&0xFF; pktHdr[11]=(inclLen>>24)&0xFF;
    pktHdr[12]=origTotal&0xFF; pktHdr[13]=(origTotal>>8)&0xFF; pktHdr[14]=(origTotal>>16)&0xFF; pktHdr[15]=(origTotal>>24)&0xFF;

    bool ok = _f.write(pktHdr, sizeof(pktHdr)) == sizeof(pktHdr)
           && _f.write(radiotap, RADIOTAP_LEN) == RADIOTAP_LEN
           && _f.write(data, len) == len;
    // Flush every frame: this is a lab capture meant to be pulled and
    // inspected later, not a high-rate logger — correctness (don't lose a
    // handshake to a mid-capture crash) matters more than write throughput
    // here, and the frame rate after the MGMT/EAPOL filter is low.
    _f.flush();
    return ok;
}

void PcapWriter::close()
{
    if (_open) {
        _f.flush();
        _f.close();
        _open = false;
    }
}

bool PcapWriter::isOpen() { return _open; }
