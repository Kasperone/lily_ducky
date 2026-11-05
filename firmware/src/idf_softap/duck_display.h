// =============================================================================
// duck_display.h - local status dashboard (LCD + APA102) for the C5 (layer 3c)
// =============================================================================
// Port of lily_ducky firmware/src/display/display.{h,cpp} to plain ESP-IDF.
// The Arduino version owns the panel through TFT_eSPI and reads its state back
// out of the Interpreter/Hal objects; here the panel is driven by st7735.c and
// the state is pushed in from main.c, which already has every value the
// dashboard shows.
//
// Layout (landscape 160x80, 10 px row pitch - same as lily):
//   ┌──────────────────────────────┐
//   │ LilyDucky              ●RUN  │  title + state dot/label
//   │ SSID:    LilyC2              │  static, painted once
//   │ IP:      192.168.4.1         │
//   │ Token:   ABC123XYZ987QRSTU   │  yellow, so the operator skips serial
//   │ Clients: 0                   │
//   │ Recon:   REC 128   (or off)  │
//   │ Line:    12/48 STRING foo    │  payload progress + current line
//   └──────────────────────────────┘
//
// Paints are diff-gated against a cached snapshot exactly like lily's
// lcdUpdate(): update() is meant to be called at ~1 Hz but only repaints the
// rows whose value actually changed, so an idle device costs nothing.
//
// The status LED is handled here too. On the C5 the APA102 shares the LCD/SD
// SPI bus with no chip select, so every paint clocks garbage through it; after
// any paint this module re-latches the intended colour (board_led_relatch()).
// =============================================================================
#ifndef C5_DUCK_DISPLAY_H
#define C5_DUCK_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Everything the dashboard renders. Filled in by main.c from the interpreter,
// the C2 hooks and the SoftAP. `interpState` uses duck_interp_state_t values
// (DUCK_INTERP_*), passed as an int so this header has no interpreter
// dependency.
typedef struct {
    int      interpState;      // DUCK_INTERP_IDLE/RUNNING/COMPLETE/ERROR/STOPPED
    int      clients;          // associated SoftAP stations
    char     ip[16];           // SoftAP IP, e.g. "192.168.4.1"
    char     token[17];        // C2 auth token snippet (full 16 chars fits)
    bool     reconCapturing;
    uint32_t reconPackets;
    int      pc;               // 0-based index of the line being executed
    int      lineCount;        // total lines in the loaded payload (0 = none)
    char     currentLine[24];  // preview of the line at pc
} duck_display_model_t;

// Initialises the panel and the status LED. Call AFTER storage_init() (or after
// board_spi_bus_init()) so the shared SPI bus already exists - the LCD is the
// second device on it. Never aborts: a missing panel leaves st7735_ready()
// false and this module draws nothing, while the LED and the rest of the
// firmware keep working.
esp_err_t duck_display_init(void);

// True when the panel came up. Log/behaviour decisions only - update() is safe
// either way.
bool duck_display_ready(void);

// Renders `m`, repainting only what changed since the last call. Returns true
// when something was actually painted, in which case the status LED was
// re-latched before returning. Cheap enough to call from the alive loop.
bool duck_display_update(const duck_display_model_t *m);

// Forces the next update() to repaint every row (used after a failed early
// init, or when the caller wants a guaranteed refresh).
void duck_display_invalidate(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_DUCK_DISPLAY_H
