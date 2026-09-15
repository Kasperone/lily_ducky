// =============================================================================
// display/display.cpp — RGB LED + ST7735 LCD status display
// =============================================================================
// LED state mapping (always active, hardware-cheap):
//   Idle     →  amber pulse        (slow, steady)
//   Running  →  green              (active typing)
//   Complete →  green solid        (hold 3s, then idle)
//   Error    →  red                (sticky until reboot)
//   Stopped  →  amber blink
//   WiFi/C2  →  cyan when clients connected
//
// LCD layout (LCD_ENABLED=1, T-Dongle-S3 80×160 ST7735, landscape 160×80):
//   ┌──────────────────────────────┐
//   │ LilyDucky              ●STATE│  ← title + colour dot
//   │ SSID:    LilyC2              │
//   │ IP:      192.168.4.1         │
//   │ Token:   ABC123XYZ987QRSTU   │  ← yellow, lets the user skip serial
//   │ Clients: 0                   │
//   │ Recon:   off                 │
//   │ Line:    12/48  PRESS GUI r  │  ← payload progress + current line
//   │ OS:WIN  LAY:US                │  ← live DETECT_OS / LAYOUT state
//   └──────────────────────────────┘
// Row pitch is 10px (8 rows in the 80px tall rotated screen) to fit the
// payload-progress rows added on top of the original 6-row dashboard.
//
// Paints are diff-gated against a cached snapshot — update() is called every
// loop() (1kHz+) but only repaints when something actually changed.
// =============================================================================

#include "display.h"
#include "config.h"
#include "c2/web_server.h"

#if LCD_ENABLED
  #include <TFT_eSPI.h>
  #include <WiFi.h>
#endif

// ─── LED state (unchanged from pre-LCD version) ───────────────────────────
static unsigned long _completeSince = 0;
static bool _errorShown = false;
// STOPPED blinks amber once (3 pulses) then holds steady — ledBlink() blocks
// ~900 ms, so without the one-shot guard every loop() tick would re-blink
// and starve C2 + button handling while the state stays "stopped".
static bool _stoppedBlinked = false;

#if LCD_ENABLED

static TFT_eSPI _tft;

// Cached snapshot — repaint only when any field changes.
struct LcdState {
    InterpState interp;
    bool        c2Running;
    int         clients;
    char        ip[16];
    char        token[17];
    bool        reconCapturing;
    uint32_t    reconPackets;
    int         pc;
    int         lineCount;
    char        currentLine[24];
    uint8_t     layoutId;
    uint8_t     osResult;
    bool        firstPaint;
};
static LcdState _last = { (InterpState)-1, false, -1, "", "", false, 0,
                           -1, -1, "", 0xFF, 0xFF, true };

// Layout constants (landscape, 160×80). 10px pitch, 8 rows — the two
// payload-progress rows (LINE/OSLAY) added below the original 6-row
// dashboard are why this is tighter than the original 12px pitch.
static const int ROW_TITLE   = 1;
static const int ROW_SSID    = 11;
static const int ROW_IP      = 21;
static const int ROW_TOKEN   = 31;
static const int ROW_CLIENTS = 41;
static const int ROW_RECON   = 51;
static const int ROW_LINE    = 61;
static const int ROW_OSLAY   = 71;
static const int ROW_H       = 9;   // field clear-rect height (font is 8px)
static const int COL_LABEL   = 2;
static const int COL_VALUE   = 50;
static const int DOT_R       = 4;
static const int DOT_X       = 152;
static const int DOT_Y       = 5;

static uint16_t stateColour(InterpState s, bool c2Running, int clients)
{
    switch (s) {
        case INTERP_RUNNING:  return TFT_GREEN;
        case INTERP_COMPLETE: return TFT_DARKGREEN;
        case INTERP_ERROR:    return TFT_RED;
        case INTERP_STOPPED:  return TFT_ORANGE;
        case INTERP_IDLE:
        default:
            if (c2Running && clients > 0) return TFT_CYAN;
            return TFT_YELLOW;
    }
}

static const char* stateLabel(InterpState s)
{
    switch (s) {
        case INTERP_RUNNING:  return "RUN";
        case INTERP_COMPLETE: return "OK ";
        case INTERP_ERROR:    return "ERR";
        case INTERP_STOPPED:  return "STP";
        case INTERP_IDLE:
        default:              return "IDL";
    }
}

// Erase the value column for one row, then write the new value.
static void writeField(int row, const char* value, uint16_t colour)
{
    _tft.fillRect(COL_VALUE, row, LCD_HEIGHT - COL_VALUE - 2, ROW_H, TFT_BLACK);
    _tft.setTextColor(colour, TFT_BLACK);
    _tft.setCursor(COL_VALUE, row);
    _tft.print(value);
}

// Like writeField but clears/writes the whole row width — used for OSLAY,
// which packs two short fields ("OS:WIN LAY:US") into one row instead of
// the label-column/value-column split the other rows use (no room left).
static void writeRow(int row, const char* value, uint16_t colour)
{
    _tft.fillRect(COL_LABEL, row, LCD_HEIGHT - COL_LABEL - 2, ROW_H, TFT_BLACK);
    _tft.setTextColor(colour, TFT_BLACK);
    _tft.setCursor(COL_LABEL, row);
    _tft.print(value);
}

static const char* osLabel(uint8_t os)
{
    switch (os) {
        case OS_WINDOWS: return "WIN";
        case OS_MACOS:   return "MAC";
        case OS_LINUX:   return "LNX";
        default:         return "?";
    }
}

static const char* layoutLabel(uint8_t id)
{
    switch (id) {
        case LAYOUT_PL: return "PL";
        case LAYOUT_DE: return "DE";
        default:        return "US";
    }
}

static void paintStaticFrame()
{
    _tft.fillScreen(TFT_BLACK);
    _tft.setTextSize(1);

    // Title (cyan)
    _tft.setTextColor(TFT_CYAN, TFT_BLACK);
    _tft.setCursor(COL_LABEL, ROW_TITLE);
    _tft.print("LilyDucky");

    // Labels (grey)
    _tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    _tft.setCursor(COL_LABEL, ROW_SSID);    _tft.print("SSID:");
    _tft.setCursor(COL_LABEL, ROW_IP);      _tft.print("IP:");
    _tft.setCursor(COL_LABEL, ROW_TOKEN);   _tft.print("Token:");
    _tft.setCursor(COL_LABEL, ROW_CLIENTS); _tft.print("Clients:");
    _tft.setCursor(COL_LABEL, ROW_RECON);   _tft.print("Recon:");
    _tft.setCursor(COL_LABEL, ROW_LINE);    _tft.print("Line:");
    // ROW_OSLAY has no static label — writeRow() below owns the whole row.

    // SSID is static — paint once
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.setCursor(COL_VALUE, ROW_SSID);
    _tft.print(CFG_WIFI_SSID);
}

static void lcdInit()
{
    _tft.init();
    _tft.setRotation(3);                 // landscape, LilyGO factory orientation
    // Backlight is active-LOW on the C5, driven with PWM at CFG_LCD_BL_LEVEL.
    // TFT_BACKLIGHT_ON carries the on-level per target (C5: LOW), so the duty
    // is inverted for active-low. (The status LED sharing this SPI bus is
    // unrelated to the backlight — see config.h CFG_LCD_BL_LEVEL.)
    analogWrite(PIN_LCD_BL,
                TFT_BACKLIGHT_ON == LOW ? 255 - CFG_LCD_BL_LEVEL : CFG_LCD_BL_LEVEL);
    paintStaticFrame();
}

// Returns true if it actually painted anything to the LCD this call. The
// caller uses that to re-latch the status LED (Hal::ledRefresh) — on the C5 the
// LED shares this SPI bus, so any paint here clocks garbage through it.
static bool lcdUpdate(InterpState s, bool c2Running, int clients,
                       bool reconCapturing, uint32_t reconPackets,
                       int pc, int lineCount, const char* currentLine,
                       uint8_t layoutId, uint8_t osResult)
{
    bool painted = false;
    // Snapshot dynamic fields. Zero-init so the firstPaint slot doesn't
    // pick up stack garbage when we copy cur → _last below.
    LcdState cur = {};
    cur.interp         = s;
    cur.c2Running      = c2Running;
    cur.clients        = clients;
    cur.reconCapturing = reconCapturing;
    cur.reconPackets   = reconPackets;
    cur.pc             = pc;
    cur.lineCount      = lineCount;
    snprintf(cur.currentLine, sizeof(cur.currentLine), "%s", currentLine ? currentLine : "");
    cur.layoutId       = layoutId;
    cur.osResult       = osResult;
    IPAddress ip   = WiFi.softAPIP();
    snprintf(cur.ip, sizeof(cur.ip), "%s", ip.toString().c_str());
    const char* tok = C2Server::authToken();
    snprintf(cur.token, sizeof(cur.token), "%s", tok ? tok : "");

    if (_last.firstPaint ||
        strcmp(_last.ip, cur.ip) != 0) {
        writeField(ROW_IP, cur.ip, TFT_WHITE);
        painted = true;
    }
    if (_last.firstPaint ||
        strcmp(_last.token, cur.token) != 0) {
        writeField(ROW_TOKEN, cur.token[0] ? cur.token : "(none)", TFT_YELLOW);
        painted = true;
    }
    if (_last.firstPaint ||
        _last.clients != cur.clients) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cur.clients);
        writeField(ROW_CLIENTS, buf, cur.clients > 0 ? TFT_GREEN : TFT_WHITE);
        painted = true;
    }
    if (_last.firstPaint ||
        _last.reconCapturing != cur.reconCapturing ||
        _last.reconPackets != cur.reconPackets) {
        char buf[16];
        if (cur.reconCapturing) snprintf(buf, sizeof(buf), "REC %lu", (unsigned long)cur.reconPackets);
        else snprintf(buf, sizeof(buf), "off");
        writeField(ROW_RECON, buf, cur.reconCapturing ? TFT_RED : TFT_DARKGREY);
        painted = true;
    }
    if (_last.firstPaint ||
        _last.interp != cur.interp ||
        _last.c2Running != cur.c2Running ||
        (_last.clients == 0) != (cur.clients == 0)) {
        // Status corner: dot + 3-char label
        uint16_t col = stateColour(cur.interp, cur.c2Running, cur.clients);
        _tft.fillCircle(DOT_X, DOT_Y, DOT_R, col);
        // Erase old label area, write new
        _tft.fillRect(DOT_X - 22, ROW_TITLE, 18, ROW_H, TFT_BLACK);
        _tft.setTextColor(col, TFT_BLACK);
        _tft.setCursor(DOT_X - 22, ROW_TITLE);
        _tft.print(stateLabel(cur.interp));
        painted = true;
    }
    if (_last.firstPaint ||
        _last.pc != cur.pc ||
        _last.lineCount != cur.lineCount ||
        strcmp(_last.currentLine, cur.currentLine) != 0) {
        // "12/48 STRING foo..." — progress plus a preview of the line at pc.
        // Off-screen overflow just gets clipped by the panel, not wrapped.
        char buf[28];
        if (cur.lineCount > 0) {
            snprintf(buf, sizeof(buf), "%d/%d %s", cur.pc + 1, cur.lineCount, cur.currentLine);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        writeField(ROW_LINE, buf, cur.interp == INTERP_RUNNING ? TFT_GREEN : TFT_WHITE);
        painted = true;
    }
    if (_last.firstPaint ||
        _last.layoutId != cur.layoutId ||
        _last.osResult != cur.osResult) {
        char buf[20];
        snprintf(buf, sizeof(buf), "OS:%s  LAY:%s", osLabel(cur.osResult), layoutLabel(cur.layoutId));
        writeRow(ROW_OSLAY, buf, TFT_WHITE);
        painted = true;
    }

    _last = cur;
    _last.firstPaint = false;
    return painted;
}

#endif // LCD_ENABLED

void Display::init()
{
    Hal::statusIdle();
    _completeSince = 0;
    _errorShown = false;
    _stoppedBlinked = false;
#if LCD_ENABLED
    lcdInit();
#endif
}

// Idle-state LED colour also used once the post-completion green window
// (see INTERP_COMPLETE below) has elapsed — kept in one place so the two
// call sites can't drift.
static void ledIdle(bool c2Running, int clients)
{
    if (c2Running && clients > 0) {
        Hal::statusWiFi();
    } else if (c2Running) {
        Hal::ledSet(0, 80, 120);
    } else {
        Hal::statusIdle();
    }
}

void Display::update(InterpState state, bool c2Running, int clients,
                      bool reconCapturing, uint32_t reconPackets,
                      int pc, int lineCount, const char* currentLine,
                      uint8_t layoutId, uint8_t osResult)
{
    // ── LED (always) ─────────────────────────────────────────────────────
    if (!_errorShown) {  // once in error mode, stay red
        switch (state) {
        case INTERP_IDLE:
            ledIdle(c2Running, clients);
            break;

        case INTERP_RUNNING:
            _stoppedBlinked = false;
            _completeSince = 0;  // fresh 3 s green window on the next completion
            Hal::statusRunning();
            break;

        case INTERP_COMPLETE:
            // _state latches at INTERP_COMPLETE until the next run() — it never
            // reverts to IDLE on its own — so this branch runs on every tick
            // for as long as the interpreter is idle-after-completion. Show
            // solid green for 3 s, then settle into the same idle colour
            // INTERP_IDLE uses, without re-zeroing _completeSince (that used to
            // re-arm the 3 s window every tick and glue the LED to green forever).
            if (_completeSince == 0) _completeSince = millis();
            if (millis() - _completeSince <= 3000) {
                Hal::statusComplete();
            } else {
                ledIdle(c2Running, clients);
            }
            break;

        case INTERP_ERROR:
            Hal::statusError();
            _errorShown = true;
            break;

        case INTERP_STOPPED:
            _completeSince = 0;
            _errorShown = false;
            if (!_stoppedBlinked) {
                _stoppedBlinked = true;
                Hal::ledBlink(255, 120, 0, 3, 150);
                Hal::ledSet(255, 120, 0);   // hold steady amber afterwards
            }
            break;
        }
    }

    // ── LCD (only when enabled — diff-gated inside) ──────────────────────
#if LCD_ENABLED
    bool painted = lcdUpdate(state, c2Running, clients, reconCapturing, reconPackets,
                              pc, lineCount, currentLine, layoutId, osResult);
    // On the C5 the APA102 shares this SPI bus, so a paint just clocked garbage
    // through the CS-less LED — re-latch the status colour. No-op elsewhere.
    if (painted) Hal::ledRefresh();
#endif
}
