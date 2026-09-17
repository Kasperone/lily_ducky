// =============================================================================
// console/console.cpp — tiny serial command interface (USB-CDC console)
// =============================================================================
#include "console.h"
#include <Arduino.h>
#include <base64.h>   // core-provided base64::encode(const uint8_t*, size_t) -> String
#include "config.h"
#include "storage/storage.h"
#include "recon/recon.h"
#include "lwip/stats.h"  // TEMP DIAGNOSTIC (open-questions.md #9): LWIPSTATS command

static char _line[80];
static size_t _lineLen = 0;

// Same validation as web_server.cpp's validName() — duplicated rather than
// shared across modules for a single three-line check used by one command;
// not worth a cross-module dependency for this.
static bool validDumpName(const String& name)
{
    int n = name.length();
    if (n == 0 || n >= CFG_MAX_PAYLOAD_FN) return false;
    if (name[0] == '.') return false;
    if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
        name.indexOf("..") >= 0) return false;
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if ((unsigned char)c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static void handleDump(const String& name)
{
    if (!validDumpName(name)) {
        Serial.println("DUMP_ERROR invalid name");
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_RECON_DIR, name.c_str());
    File f = Storage::fs().open(path);
    if (!f) {
        Serial.println("DUMP_ERROR not found");
        return;
    }

    size_t total = f.size();
    Serial.printf("DUMP_BEGIN %s %u\n", name.c_str(), (unsigned)total);

    static const size_t RAW_CHUNK = 512; // -> ~684 base64 chars/line
    uint8_t buf[RAW_CHUNK];
    int n;
    while ((n = f.read(buf, RAW_CHUNK)) > 0) {
        Serial.println(base64::encode(buf, n));
    }
    f.close();
    Serial.println("DUMP_END");
}

// `SCAN` — starts the Phase 2a managed AP scan (same Recon::startScan() the
// REST route /api/recon/scan already calls). Was previously only reachable
// over REST, which this project's own findings call unreliable — added here
// so the serial-only validation path (PMF, ENUM) actually has a way to
// produce the AP table they both depend on. No new radio behavior: this is
// the exact, already-proven 2a scan.
static void handleScan()
{
    if (!Recon::startScan()) {
        Serial.println("SCAN_ERROR radio busy");
    }
}

// `ENUM <ap-index>` — Phase 2b station enumeration. <ap-index> is a row
// number into the last SCAN's AP table (0-based, same order as the
// `[RECON]` scan lines). Kicks off a bounded passive sniff on that AP's
// channel; results stream as `[RECON-STA]` lines from Recon::tick().
static void handleEnum(const String& arg)
{
    // toInt() returns 0 on anything non-numeric, including "" — a bad index
    // (like "0" when there are no APs, or an out-of-range one) is already
    // rejected by Recon::startEnum() itself, so no separate parse-error path.
    long idx = arg.toInt();
    if (idx < 0) {
        Serial.println("ENUM_ERROR invalid index");
        return;
    }
    if (!Recon::startEnum((uint32_t)idx)) {
        Serial.println("ENUM_ERROR radio busy or index out of range");
    }
}

// `PMF` — explicit Phase 2b PMF sweep trigger. A plain SCAN already
// auto-chains this (CFG_RECON_AUTO_PMF_SWEEP defaults ON — see config.h);
// this command exists to re-run the sweep standalone (e.g. after PMFDOWN's
// DFS pass, or without a fresh SCAN). Sweeps the channels from the last
// completed SCAN's AP table; results stream as `[RECON]` lines from
// Recon::tick(), same as the auto-chained run.
static void handlePmf()
{
    if (!Recon::startPmfSweep()) {
        Serial.println("PMF_ERROR radio busy or no scan results yet");
    }
}

// `PMFDOWN` — investigation-gated AP-down DFS-capable PMF sweep. Tears the
// SoftAP down, sweeps every channel from the last SCAN (including DFS
// channels PMF can't reach with the AP up), then restores the SoftAP.
// Explicit-only, never auto-triggered — PMF/SCAN's AP-up paths are
// unaffected by this existing. See recon.h and AGENTS.md's Module B note.
static void handlePmfDown()
{
    if (!Recon::startPmfSweepApDown()) {
        Serial.println("PMFDOWN_ERROR radio busy or no scan results yet");
    }
}

// `LWIPSTATS` — dumps lwIP's internal pbuf/memp/proto counters to serial.
// CONFIG_LWIP_STATS=y (platformio.ini) enables collection; this triggers
// stats_display() (lwIP's own dump, routed through its platform diag macro
// to this same serial console) on demand. Added while investigating
// open-questions.md #9 (C5 SoftAP goes deaf for a real external client after
// one request) — kept as a permanent tool, not reverted, since it already
// proved useful there: it showed zero drops/errors anywhere in lwIP's own
// accounting during a live reproduction, which is exactly the kind of
// on-device evidence that's otherwise impossible to get without a debugger.
static void handleLwipStats()
{
    Serial.println("[LWIPSTATS] ---- begin ----");
    stats_display();
    Serial.println("[LWIPSTATS] ---- end ----");
}

static void dispatch(const String& line)
{
    if (line.startsWith("DUMP ")) {
        handleDump(line.substring(5));
    } else if (line == "SCAN") {
        handleScan();
    } else if (line.startsWith("ENUM ")) {
        handleEnum(line.substring(5));
    } else if (line == "PMFDOWN") {
        handlePmfDown();
    } else if (line == "PMF") {
        handlePmf();
    } else if (line == "LWIPSTATS") {
        handleLwipStats();
    }
    // Unrecognized lines are ignored — this console shares the port with
    // the normal boot/status log, so silently ignoring stray input (rather
    // than erroring) keeps it from reacting to anything but an exact match.
}

void Console::tick()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (_lineLen > 0) {
                _line[_lineLen] = '\0';
                dispatch(String(_line));
                _lineLen = 0;
            }
            continue;
        }
        if (_lineLen < sizeof(_line) - 1) {
            _line[_lineLen++] = c;
        }
    }
}
