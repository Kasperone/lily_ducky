// =============================================================================
// st7735.h - minimal pure ESP-IDF ST7735 driver (T-Dongle-C5 0.96" 80x160 IPS)
// =============================================================================
// Deliberately tiny: this replaces TFT_eSPI for exactly the operations the
// status dashboard needs - init, orientation, full-screen fill, rect fill and
// a 5x7 text painter. No Arduino.h, no TFT_eSPI, no bit-banging.
//
// Panel: ST7735R with the "green tab 160x80" geometry, driven in landscape as
// 160x80 (rotation 3, lily's setRotation(3)). BGR colour order and display
// inversion are both ON, per lily's build flags (TFT_RGB_ORDER=TFT_BGR,
// TFT_INVERSION_ON=1) - these are panel-batch properties, not driver bugs.
//
// Bus: a spi_device on the shared SPI2_HOST owned by board_spi.c. Every public
// entry point takes the board mutex for its whole duration, so an SD transfer
// can never be interleaved into the middle of a CASET/RASET/RAMWR sequence.
// =============================================================================
#ifndef C5_ST7735_H
#define C5_ST7735_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 16-bit 5-6-5 colour, the way a TFT expects it. Values are named to match the
// TFT_eSPI constants lily's display.cpp uses, so the dashboard code reads the
// same. NOTE: the panel is BGR-ordered in hardware (MADCTL BGR bit), so a
// "red" here comes out red on screen.
#define ST7735_BLACK       0x0000
#define ST7735_WHITE       0xFFFF
#define ST7735_RED         0xF800
#define ST7735_GREEN       0x07E0
#define ST7735_DARKGREEN   0x03E0
#define ST7735_CYAN        0x07FF
#define ST7735_YELLOW      0xFFE0
#define ST7735_ORANGE      0xFD20
#define ST7735_DARKGREY    0x7BEF

// Brings up the panel: reset, ST7735R init sequence, landscape orientation,
// backlight PWM on. Requires board_spi_bus_init() to have succeeded first (the
// LCD is the second device on the shared bus). Idempotent.
esp_err_t st7735_init(void);

// True once st7735_init() succeeded.
bool st7735_ready(void);

// ── Primitives ──────────────────────────────────────────────────────────────
// All coordinates are in the landscape 160x80 frame. Out-of-range rectangles
// are clipped; a rectangle that ends up empty is a no-op.
void st7735_fill_screen(uint16_t colour);
void st7735_fill_rect(int x, int y, int w, int h, uint16_t colour);

// Draws NUL-terminated 8-bit text at (x,y) with an opaque background. Only
// printable ASCII 0x20..0x7E has glyphs; anything else is drawn blank. Text
// running past the right edge is clipped, never wrapped (same as lily, where
// the panel just clipped it).
// `fontScale` >= 1: 1 is the 5x7 cell (6x8 advance), 2 doubles it.
void st7735_draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                        uint8_t fontScale);

// Clear rect (x,y,w,h) to `bg` and draw `s` into it as one atomic bus
// operation. This is the form a diff-gated dashboard row refresh should use:
// st7735_fill_rect() followed by st7735_draw_string() is two separate bus
// sequences, and SD traffic could be interleaved between them, leaving the row
// half-erased until the next update.
void st7735_draw_field(int x, int y, int w, int h, const char *s, uint16_t fg,
                       uint16_t bg, uint8_t fontScale);

// Frame-level arbitration. Taking the bus mutex for a whole multi-row repaint
// keeps a partial frame off the wire and - just as important - makes the LED
// re-latch that follows it meaningful (there is no window for another device to
// clock garbage through the CS-less APA102 after we finish). The lock is
// recursive, so the individual primitives may still be called inside a batch.
// Every exit path must reach st7735_batch_end().
void st7735_batch_begin(void);
void st7735_batch_end(void);

// Filled status dot, clipped to the frame. Used for the dashboard's state
// indicator in the title row.
void st7735_fill_circle(int cx, int cy, int r, uint16_t colour);

// Character cell size for a given scale, so layout code can position rows.
#define ST7735_CHAR_W 6   // 5 px glyph + 1 px spacing
#define ST7735_CHAR_H 8   // 7 px glyph + 1 px descender row

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_ST7735_H
