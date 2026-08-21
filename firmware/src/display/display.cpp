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
//   │ Lily-Ducky              ●STATE│  ← title + colour dot
//   │ SSID:    LilyC2              │
//   │ IP:      192.168.4.1         │
//   │ Token:   ABC123XYZ987QRSTU   │  ← yellow, lets the user skip serial
//   │ Clients: 0                   │
//   └──────────────────────────────┘
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

#if LCD_ENABLED

static TFT_eSPI _tft;

// Cached snapshot — repaint only when any field changes.
struct LcdState {
    InterpState interp;
    bool        c2Running;
    int         clients;
    char        ip[16];
    char        token[17];
    bool        firstPaint;
};
static LcdState _last = { (InterpState)-1, false, -1, "", "", true };

// Layout constants (landscape, 160×80)
static const int ROW_TITLE   = 2;
static const int ROW_SSID    = 20;
static const int ROW_IP      = 32;
static const int ROW_TOKEN   = 44;
static const int ROW_CLIENTS = 56;
static const int COL_LABEL   = 2;
static const int COL_VALUE   = 50;
static const int DOT_R       = 4;
static const int DOT_X       = 152;
static const int DOT_Y       = 6;

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
    _tft.fillRect(COL_VALUE, row, LCD_HEIGHT - COL_VALUE - 2, 10, TFT_BLACK);
    _tft.setTextColor(colour, TFT_BLACK);
    _tft.setCursor(COL_VALUE, row);
    _tft.print(value);
}

static void paintStaticFrame()
{
    _tft.fillScreen(TFT_BLACK);
    _tft.setTextSize(1);

    // Title (cyan)
    _tft.setTextColor(TFT_CYAN, TFT_BLACK);
    _tft.setCursor(COL_LABEL, ROW_TITLE);
    _tft.print("Lily-Ducky");

    // Labels (grey)
    _tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    _tft.setCursor(COL_LABEL, ROW_SSID);    _tft.print("SSID:");
    _tft.setCursor(COL_LABEL, ROW_IP);      _tft.print("IP:");
    _tft.setCursor(COL_LABEL, ROW_TOKEN);   _tft.print("Token:");
    _tft.setCursor(COL_LABEL, ROW_CLIENTS); _tft.print("Clients:");

    // SSID is static — paint once
    _tft.setTextColor(TFT_WHITE, TFT_BLACK);
    _tft.setCursor(COL_VALUE, ROW_SSID);
    _tft.print(CFG_WIFI_SSID);
}

static void lcdInit()
{
    _tft.init();
    _tft.setRotation(1);                 // landscape: USB connector right
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);
    paintStaticFrame();
}

static void lcdUpdate(InterpState s, bool c2Running, int clients)
{
    // Snapshot dynamic fields. Zero-init so the firstPaint slot doesn't
    // pick up stack garbage when we copy cur → _last below.
    LcdState cur = {};
    cur.interp     = s;
    cur.c2Running  = c2Running;
    cur.clients    = clients;
    IPAddress ip   = WiFi.softAPIP();
    snprintf(cur.ip, sizeof(cur.ip), "%s", ip.toString().c_str());
    const char* tok = WebServer::authToken();
    snprintf(cur.token, sizeof(cur.token), "%s", tok ? tok : "");

    if (_last.firstPaint ||
        strcmp(_last.ip, cur.ip) != 0) {
        writeField(ROW_IP, cur.ip, TFT_WHITE);
    }
    if (_last.firstPaint ||
        strcmp(_last.token, cur.token) != 0) {
        writeField(ROW_TOKEN, cur.token[0] ? cur.token : "(none)", TFT_YELLOW);
    }
    if (_last.firstPaint ||
        _last.clients != cur.clients) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cur.clients);
        writeField(ROW_CLIENTS, buf, cur.clients > 0 ? TFT_GREEN : TFT_WHITE);
    }
    if (_last.firstPaint ||
        _last.interp != cur.interp ||
        _last.c2Running != cur.c2Running ||
        (_last.clients == 0) != (cur.clients == 0)) {
        // Status corner: dot + 3-char label
        uint16_t col = stateColour(cur.interp, cur.c2Running, cur.clients);
        _tft.fillCircle(DOT_X, DOT_Y, DOT_R, col);
        // Erase old label area, write new
        _tft.fillRect(DOT_X - 22, ROW_TITLE, 18, 10, TFT_BLACK);
        _tft.setTextColor(col, TFT_BLACK);
        _tft.setCursor(DOT_X - 22, ROW_TITLE);
        _tft.print(stateLabel(cur.interp));
    }

    _last = cur;
    _last.firstPaint = false;
}

#endif // LCD_ENABLED

void Display::init()
{
    Hal::statusIdle();
    _completeSince = 0;
    _errorShown = false;
#if LCD_ENABLED
    lcdInit();
#endif
}

void Display::update(InterpState state, bool c2Running, int clients)
{
    // ── LED (always) ─────────────────────────────────────────────────────
    if (!_errorShown) {  // once in error mode, stay red
        switch (state) {
        case INTERP_IDLE:
            if (c2Running && clients > 0) {
                Hal::statusWiFi();
            } else if (c2Running) {
                Hal::ledSet(0, 80, 120);
            } else {
                Hal::statusIdle();
            }
            break;

        case INTERP_RUNNING:
            Hal::statusRunning();
            break;

        case INTERP_COMPLETE:
            if (_completeSince == 0) _completeSince = millis();
            Hal::statusComplete();
            if (millis() - _completeSince > 3000) {
                _completeSince = 0;
                Hal::statusIdle();
            }
            break;

        case INTERP_ERROR:
            Hal::statusError();
            _errorShown = true;
            break;

        case INTERP_STOPPED:
            _completeSince = 0;
            _errorShown = false;
            Hal::ledBlink(255, 120, 0, 3, 150);
            break;
        }
    }

    // ── LCD (only when enabled — diff-gated inside) ──────────────────────
#if LCD_ENABLED
    lcdUpdate(state, c2Running, clients);
#endif
}
