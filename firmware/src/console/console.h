// =============================================================================
// console/console.h — tiny serial command interface (USB-CDC console)
// =============================================================================
// Currently one command: `DUMP <file>` — base64-dumps a file from
// SD_RECON_DIR to Serial, wrapped in DUMP_BEGIN/DUMP_END markers. Exists to
// pull recon captures off the SD card over the wired USB-CDC link when the
// SoftAP can't sustain a large HTTP transfer (see the pcap download route's
// known sustained-TX ceiling — this sidesteps WiFi entirely).
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
