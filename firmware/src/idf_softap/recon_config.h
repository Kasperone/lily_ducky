// =============================================================================
// recon_config.h - CFG_RECON_* constants for the C5 recon port (layer 3c)
// =============================================================================
// Only the recon tunables are copied from lily_ducky's firmware/src/config.h
// (Module B block). Values are identical to lily's where the behaviour is the
// same, so capture/scan/enum timing does not drift:
//
//   CFG_RECON_SNAPLEN           400 B per frame  (radiotap + 802.11 header;
//                               covers mgmt frames and EAPOL)
//   CFG_RECON_RING_SLOTS        32 slots between the promiscuous RX callback
//                               and the tick task
//   CFG_RECON_SCAN_MAX_APS      32 APs in the RAM table
//   CFG_RECON_SCAN_DWELL_MS     300 ms max per channel during the scan
//   CFG_RECON_PMF_DWELL_MS      250 ms per channel during the PMF sweep
//   CFG_RECON_MAX_STAS          16 stations in the enumeration table
//   CFG_RECON_ENUM_DWELL_MS     4000 ms per ENUM attempt
//   CFG_RECON_ENUM_MAX_ATTEMPTS 2 (one retry when 0 stations were seen)
//
// Deliberately NOT ported: CFG_RECON_APDOWN_SETTLE_MS and the whole AP-down
// PMF sweep. That path tears the SoftAP down and needs a SoftAP restore that
// hung the reference firmware's main loop (see recon.h in lily and issue #9
// here). This tree stubs startPmfSweepApDown() permanently false - no code
// path can take the SoftAP down.
//
// Also not ported: CFG_RECON_AUTO_PMF_SWEEP. On this build the PMF sweep hops
// the radio off the SoftAP's channel, which is exactly the disturbance issue
// #9 warns about, so a scan does NOT auto-chain it. PMF status is the
// best-effort authmode heuristic (WPA3 => capable), which is what the recon
// spec allows.
// =============================================================================
#ifndef C5_RECON_CONFIG_H
#define C5_RECON_CONFIG_H

// Bytes captured per frame. Sized so a full 802.11 header + RSN IE fits, which
// is what the PMF heuristic and the station-MAC resolver need to read.
#define CFG_RECON_SNAPLEN 400

// Ring depth between the promiscuous callback (producer, runs in the Wi-Fi
// driver task) and the recon tick task (consumer). At 400 B/slot plus metadata
// this is ~14 KiB of .bss - affordable on the C5's internal SRAM and deep
// enough that a slow SD flush does not drop beacons.
#define CFG_RECON_RING_SLOTS 32

// AP table capacity (RAM-resident). lily's value; each row is ~64 B.
#define CFG_RECON_SCAN_MAX_APS 32

// Max dwell per channel during the managed scan. Kept short: every millisecond
// here is a millisecond the radio is away from the SoftAP's home channel.
#define CFG_RECON_SCAN_DWELL_MS 300

// Per-channel dwell while sweeping for beacons/probe-responses (PMF).
#define CFG_RECON_PMF_DWELL_MS 250

// Station table capacity (RAM-resident).
#define CFG_RECON_MAX_STAS 16

// Dwell per ENUM attempt.
#define CFG_RECON_ENUM_DWELL_MS 4000

// If an ENUM attempt ends with 0 stations found, extend once for another full
// dwell before finishing. Hardware-observed on the reference board: a genuine
// station was missed on the first dwell and found on an immediate identical
// retry - a channel-settle/traffic-cadence gap, not a logic bug. 2 = one retry.
#define CFG_RECON_ENUM_MAX_ATTEMPTS 2

// Tick task cadence. The recon tick drains the ring, advances the scan/PMF/enum
// state machines and writes PCAP data. 20 ms keeps PCAP flushing responsive
// without spinning a whole core.
#define CFG_RECON_TICK_MS 20

// Task stack. The tick task's deepest path is PCAP formatting (~450 B frame
// copy) plus newlib's stdio on a FATFS write; 4 KiB is comfortable and this is
// a low-priority task, so it does not compete with WiFi.
#define CFG_RECON_TASK_STACK 4096

#endif  // C5_RECON_CONFIG_H
