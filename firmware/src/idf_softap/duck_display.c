// =============================================================================
// duck_display.c - ST7735 status dashboard + APA102 status colour (layer 3c)
// =============================================================================
#include "duck_display.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "board_pins.h"
#include "board_spi.h"
#include "duckcfg.h"
#include "interpreter.h"
#include "softap_secret.h"
#include "st7735.h"

static const char *TAG = "display";

// ── Layout (landscape 160x80, 10 px pitch, 8 rows) ──────────────────────────
// Same geometry as lily's display.cpp: the font cell is 6x8, the row pitch is
// 10 px so eight rows fit in 80 px, and each row's value column is cleared to
// the right edge before the new text is drawn.
static const int ROW_TITLE   = 1;
static const int ROW_SSID    = 11;
static const int ROW_IP      = 21;
static const int ROW_TOKEN   = 31;
static const int ROW_CLIENTS = 41;
static const int ROW_RECON   = 51;
static const int ROW_LINE    = 61;
static const int ROW_H       = 9;    // clear-rect height (font is 8 px)
static const int COL_LABEL   = 2;
static const int COL_VALUE   = 50;
static const int DOT_X       = 152;
static const int DOT_Y       = 5;
static const int DOT_R       = 4;

// Cached snapshot. `firstPaint` forces the first update() to write every field.
typedef struct {
    int      interp;
    int      clients;
    char     ip[16];
    char     token[17];
    bool     reconCapturing;
    uint32_t reconPackets;
    int      pc;
    int      lineCount;
    char     currentLine[24];
    bool     firstPaint;
} lcd_cache_t;

static lcd_cache_t s_last = {
    .interp = -1, .clients = -1, .ip = "", .token = "",
    .reconCapturing = false, .reconPackets = 0, .pc = -1, .lineCount = -1,
    .currentLine = "", .firstPaint = true,
};

static bool s_ready;

// ── LED mapping (identical colours to lily's Display::update) ───────────────
static const char *state_label(int s)
{
    switch (s) {
        case DUCK_INTERP_RUNNING:  return "RUN";
        case DUCK_INTERP_COMPLETE: return "OK ";
        case DUCK_INTERP_ERROR:    return "ERR";
        case DUCK_INTERP_STOPPED:  return "STP";
        case DUCK_INTERP_IDLE:
        default:                   return "IDL";
    }
}

static uint16_t state_colour(int s, int clients)
{
    switch (s) {
        case DUCK_INTERP_RUNNING:  return ST7735_GREEN;
        case DUCK_INTERP_COMPLETE: return ST7735_DARKGREEN;
        case DUCK_INTERP_ERROR:    return ST7735_RED;
        case DUCK_INTERP_STOPPED:  return ST7735_ORANGE;
        case DUCK_INTERP_IDLE:
        default:
            // Idle with a client attached reads teal - the same "WiFi up and
            // someone is using it" cue as lily's Hal::statusWiFi().
            if (clients > 0) return ST7735_CYAN;
            return ST7735_YELLOW;
    }
}

// Drives the APA102: state colour, with "SoftAP up + client present" showing
// teal when the interpreter is idle. Steady colours only - no blocking blink
// loops, because this runs on the alive loop and must never stall it.
static void led_for_state(int s, int clients)
{
    switch (s) {
        case DUCK_INTERP_RUNNING:  board_led_status_running();  break;
        case DUCK_INTERP_COMPLETE: board_led_status_complete(); break;
        case DUCK_INTERP_ERROR:    board_led_status_error();    break;
        case DUCK_INTERP_STOPPED:  board_led_set(255, 120, 0);  break;  // amber
        case DUCK_INTERP_IDLE:
        default:
            if (clients > 0) board_led_status_wifi();   // teal
            else             board_led_status_idle();   // amber
            break;
    }
}

// Erase the value column for one row, then write the new value - one atomic
// bus operation so an SD burst cannot leave the row half-erased.
static void write_field(int row, const char *value, uint16_t colour)
{
    st7735_draw_field(COL_VALUE, row, BOARD_LCD_WIDTH - COL_VALUE - 2, ROW_H,
                      value, colour, ST7735_BLACK, 1);
}

static void paint_static_frame(void)
{
    st7735_fill_screen(ST7735_BLACK);

    st7735_draw_string(COL_LABEL, ROW_TITLE, "LilyDucky", ST7735_CYAN,
                       ST7735_BLACK, 1);

    // Labels (grey). ROW_TITLE's right side and the whole of ROW_TITLE are the
    // state dot/label, written by the diff block below.
    static const struct { int row; const char *text; } labels[] = {
        { ROW_SSID,    "SSID:" },
        { ROW_IP,      "IP:" },
        { ROW_TOKEN,   "Token:" },
        { ROW_CLIENTS, "Clients:" },
        { ROW_RECON,   "Recon:" },
        { ROW_LINE,    "Line:" },
    };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        st7735_draw_string(COL_LABEL, labels[i].row, labels[i].text,
                           ST7735_DARKGREY, ST7735_BLACK, 1);
    }

    // SSID is static: paint it once here rather than diffing it every update.
    st7735_draw_string(COL_VALUE, ROW_SSID, CFG_WIFI_SSID, ST7735_WHITE,
                       ST7735_BLACK, 1);
}

// ── Public API ──────────────────────────────────────────────────────────────
esp_err_t duck_display_init(void)
{
    if (s_ready) return ESP_OK;
    if (board_spi_bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "shared SPI bus unavailable, panel disabled");
        return ESP_FAIL;
    }

    esp_err_t err = st7735_init();
    if (err != ESP_OK) {
        // Not fatal: the device still serves C2, and the LED works as long as
        // the bus itself came up.
        ESP_LOGE(TAG, "ST7735 init failed (%s), dashboard disabled",
                 esp_err_to_name(err));
        s_last.firstPaint = true;
        led_for_state(DUCK_INTERP_IDLE, 0);
        return err;
    }

    paint_static_frame();
    s_last.firstPaint = true;   // first update() fills every dynamic row
    s_ready = true;
    led_for_state(DUCK_INTERP_IDLE, 0);
    // The static frame was painted over the shared bus, so re-latch the LED.
    board_led_relatch();
    ESP_LOGI(TAG, "dashboard up (%dx%d, ssid=%s)", BOARD_LCD_WIDTH,
             BOARD_LCD_HEIGHT, CFG_WIFI_SSID);
    return ESP_OK;
}

bool duck_display_ready(void) { return s_ready; }

void duck_display_invalidate(void) { s_last.firstPaint = true; }

bool duck_display_update(const duck_display_model_t *m)
{
    if (!m) return false;

    // ── LED (always, panel or not) ──────────────────────────────────────────
    led_for_state(m->interpState, m->clients);

    if (!s_ready) return false;

    // Snapshot the dynamic fields.
    lcd_cache_t cur;
    memset(&cur, 0, sizeof(cur));
    cur.interp         = m->interpState;
    cur.clients        = m->clients;
    cur.reconCapturing = m->reconCapturing;
    cur.reconPackets   = m->reconPackets;
    cur.pc             = m->pc;
    cur.lineCount      = m->lineCount;
    snprintf(cur.ip, sizeof(cur.ip), "%s", m->ip);
    snprintf(cur.token, sizeof(cur.token), "%s", m->token);
    snprintf(cur.currentLine, sizeof(cur.currentLine), "%s", m->currentLine);

    bool painted = false;
    const bool first = s_last.firstPaint;

    // One lock for the whole row set: a diff-gated repaint must reach the panel
    // as a unit, and the LED re-latch below only means anything if no other
    // device could have touched the bus between the last paint and that latch.
    st7735_batch_begin();

    if (first || strcmp(s_last.ip, cur.ip) != 0) {
        write_field(ROW_IP, cur.ip[0] ? cur.ip : "?", ST7735_WHITE);
        painted = true;
    }
    if (first || strcmp(s_last.token, cur.token) != 0) {
        write_field(ROW_TOKEN, cur.token[0] ? cur.token : "(none)",
                    ST7735_YELLOW);
        painted = true;
    }
    if (first || s_last.clients != cur.clients) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", cur.clients);
        write_field(ROW_CLIENTS, buf, cur.clients > 0 ? ST7735_GREEN
                                                      : ST7735_WHITE);
        painted = true;
    }
    if (first || s_last.reconCapturing != cur.reconCapturing ||
        s_last.reconPackets != cur.reconPackets) {
        char buf[20];
        if (cur.reconCapturing) {
            snprintf(buf, sizeof(buf), "REC %lu", (unsigned long)cur.reconPackets);
        } else {
            snprintf(buf, sizeof(buf), "off");
        }
        write_field(ROW_RECON, buf, cur.reconCapturing ? ST7735_RED
                                                       : ST7735_DARKGREY);
        painted = true;
    }
    if (first || s_last.interp != cur.interp ||
        s_last.clients != cur.clients) {
        // Status corner: filled dot + 3-char label, matching lily's layout.
        uint16_t col = state_colour(cur.interp, cur.clients);
        st7735_fill_circle(DOT_X, DOT_Y, DOT_R, col);
        st7735_fill_rect(DOT_X - 22, ROW_TITLE, 18, ROW_H, ST7735_BLACK);
        st7735_draw_string(DOT_X - 22, ROW_TITLE, state_label(cur.interp), col,
                           ST7735_BLACK, 1);
        painted = true;
    }
    if (first || s_last.pc != cur.pc || s_last.lineCount != cur.lineCount ||
        strcmp(s_last.currentLine, cur.currentLine) != 0) {
        // "12/48 STRING foo" - progress plus a preview of the line at pc.
        // Off-screen overflow is clipped by the panel, not wrapped.
        char buf[28];
        if (cur.lineCount > 0) {
            snprintf(buf, sizeof(buf), "%d/%d %s", cur.pc + 1, cur.lineCount,
                     cur.currentLine);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        write_field(ROW_LINE, buf,
                    cur.interp == DUCK_INTERP_RUNNING ? ST7735_GREEN
                                                      : ST7735_WHITE);
        painted = true;
    }

    s_last = cur;
    s_last.firstPaint = false;

    st7735_batch_end();

    // The APA102 has no chip select, so every byte above also went through it.
    // Re-latch the status colour whenever we painted anything.
    if (painted) board_led_relatch();
    return painted;
}
