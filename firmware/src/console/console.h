// =============================================================================
// console/console.h — tiny serial command interface (USB-CDC console)
// =============================================================================
// Five commands:
//   DUMP <file>   — base64-dumps a file from SD_RECON_DIR to Serial, wrapped
//                    in DUMP_BEGIN/DUMP_END markers. Exists to pull recon
//                    captures off the SD card over the wired USB-CDC link
//                    when the SoftAP can't sustain a large HTTP transfer
//                    (see the pcap download route's known sustained-TX
//                    ceiling — this sidesteps WiFi entirely).
//   SCAN          — Phase 2a managed AP scan (Recon::startScan()) — the same
//                    function the REST route /api/recon/scan calls. Added
//                    here (previously serial had no trigger for it at all)
//                    so PMF/ENUM below have a serial-only way to get the AP
//                    table they depend on. Results stream as [RECON] lines.
//   PMF           — Phase 2b: sweeps the channels from the last completed
//                    SCAN's AP table and parses each AP's real PMF status
//                    from its RSN IE. Explicit-only by default (see
//                    CFG_RECON_AUTO_PMF_SWEEP in config.h). Results stream
//                    as [RECON] lines.
//   ENUM <index>  — Phase 2b station enumeration for AP <index> from the
//                    last SCAN's table (see recon/recon.h). Results stream
//                    as [RECON-STA] lines.
//   PMFDOWN       — investigation-gated: tears the SoftAP down, sweeps every
//                    channel from the last SCAN (including DFS channels PMF
//                    can't reach with the AP up), then restores the SoftAP.
//                    Explicit-only, never auto-triggered — SCAN/PMF's AP-up
//                    paths are completely unaffected. See recon/recon.h and
//                    AGENTS.md's Module B note for the root-cause analysis
//                    behind this design (deferred/tick-based, settling
//                    delays, narrowest primitives).
//
// No auth on these commands: Serial access is already this firmware's
// trust boundary — the C2 auth token itself is only ever printed here,
// never sent over WiFi (c2/web_server.cpp) — so anyone who can read that
// token off serial can already do everything the C2 API can do anyway.
// =============================================================================
#ifndef LILY_DUCKY_CONSOLE_H
#define LILY_DUCKY_CONSOLE_H

namespace Console {

    // Reads Serial input a line at a time and dispatches commands. Call
    // every loop().
    void tick();

} // namespace Console

#endif
