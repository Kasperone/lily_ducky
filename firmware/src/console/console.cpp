// =============================================================================
// console/console.cpp — tiny serial command interface (USB-CDC console)
// =============================================================================
#include "console.h"
#include <Arduino.h>
#include <base64.h>   // core-provided base64::encode(const uint8_t*, size_t) -> String
#include "config.h"
#include "storage/storage.h"

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

static void dispatch(const String& line)
{
    if (line.startsWith("DUMP ")) {
        handleDump(line.substring(5));
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
