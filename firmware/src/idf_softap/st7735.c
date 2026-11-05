// =============================================================================
// st7735.c - minimal pure ESP-IDF ST7735 driver (see st7735.h)
// =============================================================================
// The init sequence is the standard ST7735R "green tab" one that lily gets from
// TFT_eSPI (TFT_Drivers/ST7735_Init.h: Rcmd1 -> Rcmd2green -> INVON -> Rcmd3),
// with TFT_eSPI's rotation-3 geometry for the GREENTAB160x80 variant applied
// afterwards:
//
//   MADCTL   = MX | MV | BGR   (0x68)
//   colstart = 1     rowstart = 26
//   width    = 160   height   = 80
//
// Those offsets are the panel's CGRAM origin, NOT a driver bug: the 80x160
// visible window sits inside 132x162 of controller RAM, so every address we
// send has to be shifted by (1, 26). Getting them wrong is the classic
// "image shifted/wrapped a few pixels" ST7735 symptom.
//
// Display inversion is left ON (INVON) and colour order BGR, both matching
// lily's build flags. Do not "fix" swapped colours by remapping channels here.
// =============================================================================
#include "st7735.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "board_pins.h"
#include "board_spi.h"

static const char *TAG = "st7735";

// ── ST7735 command set (only what this driver issues) ───────────────────────
#define CMD_SWRESET 0x01
#define CMD_SLPOUT  0x11
#define CMD_NORON   0x13
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A
#define CMD_FRMCTR1 0xB1
#define CMD_FRMCTR2 0xB2
#define CMD_FRMCTR3 0xB3
#define CMD_INVCTR  0xB4
#define CMD_PWCTR1  0xC0
#define CMD_PWCTR2  0xC1
#define CMD_PWCTR3  0xC2
#define CMD_PWCTR4  0xC3
#define CMD_PWCTR5  0xC4
#define CMD_VMCTR1  0xC5
#define CMD_GMCTRP1 0xE0
#define CMD_GMCTRN1 0xE1

// MADCTL bits
#define MAD_MY 0x80
#define MAD_MX 0x40
#define MAD_MV 0x20
#define MAD_BGR 0x08

// Rotation 3 (landscape) for the GREENTAB160x80 panel.
#define LCD_MADCTL_ROT3 (MAD_MX | MAD_MV | MAD_BGR)   // 0x68
#define LCD_COLSTART 1
#define LCD_ROWSTART 26

static spi_device_handle_t s_lcd;
static bool s_ready;

// ── Low-level transfers ─────────────────────────────────────────────────────
// DC is driven by GPIO between transactions. That is safe here only because
// every caller holds the board mutex for its whole sequence - nothing else can
// touch DC or the bus in between. (The LCD is the only device with a DC line,
// so no other device is affected either way.)
static void lcd_mode_cmd(void)
{
    gpio_set_level(BOARD_LCD_DC, 0);
}

static void lcd_mode_data(void)
{
    gpio_set_level(BOARD_LCD_DC, 1);
}

static void lcd_write_raw(const uint8_t *data, size_t len)
{
    if (!len) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    esp_err_t err = spi_device_transmit(s_lcd, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spi tx failed: %s", esp_err_to_name(err));
    }
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_mode_cmd();
    lcd_write_raw(&cmd, 1);
    lcd_mode_data();
}

static void lcd_data(const uint8_t *data, size_t len)
{
    lcd_mode_data();
    lcd_write_raw(data, len);
}

static void lcd_data1(uint8_t v) { lcd_data(&v, 1); }

// ── Address window ──────────────────────────────────────────────────────────
// x/y/w/h are already clipped to the 160x80 frame by the caller.
static void lcd_set_window(int x, int y, int w, int h)
{
    uint16_t xs = (uint16_t)(x + LCD_COLSTART);
    uint16_t xe = (uint16_t)(x + w - 1 + LCD_COLSTART);
    uint16_t ys = (uint16_t)(y + LCD_ROWSTART);
    uint16_t ye = (uint16_t)(y + h - 1 + LCD_ROWSTART);

    uint8_t caset[4] = { (uint8_t)(xs >> 8), (uint8_t)xs,
                         (uint8_t)(xe >> 8), (uint8_t)xe };
    uint8_t raset[4] = { (uint8_t)(ys >> 8), (uint8_t)ys,
                         (uint8_t)(ye >> 8), (uint8_t)ye };

    lcd_cmd(CMD_CASET);
    lcd_data(caset, sizeof(caset));
    lcd_cmd(CMD_RASET);
    lcd_data(raset, sizeof(raset));
    lcd_cmd(CMD_RAMWR);
}

// Pushes `count` pixels already laid out high-byte-first in `be`. Chunked so no
// single transfer exceeds the bus's max_transfer_sz.
static void lcd_push_px(const uint8_t *be, size_t count)
{
    size_t off = 0;
    while (off < count) {
        size_t chunk = count - off;
        size_t maxPx = BOARD_LCD_CHUNK_BYTES / 2;
        if (chunk > maxPx) chunk = maxPx;
        lcd_write_raw(be + off * 2, chunk * 2);
        off += chunk;
    }
}

// ── Lock-free internals ─────────────────────────────────────────────────────
// Every public entry point takes the board mutex once around a complete
// operation. The helpers below assume it is already held, which is what lets
// st7735_draw_field() clear and paint a dashboard row without an SD transfer
// being able to slip in between the two.
//
// Colour byte order: ST7735 pixel data is 16-bit RGB565 sent MOST-significant
// byte first. IDF's SPI shifts each buffer byte out MSB-first, so the 5-6-5
// value goes into the buffer verbatim - high byte at the lower offset - with no
// byte swapping anywhere. (TFT_eSPI does the same: tft_Write_8(color >> 8)
// then tft_Write_8(color).)
static inline void put_px(uint8_t *dst, uint16_t colour)
{
    dst[0] = (uint8_t)(colour >> 8);
    dst[1] = (uint8_t)(colour & 0xFF);
}

static void fill_rect_locked(int x, int y, int w, int h, uint16_t colour)
{
    // Clip to the landscape frame.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BOARD_LCD_WIDTH) w = BOARD_LCD_WIDTH - x;
    if (y + h > BOARD_LCD_HEIGHT) h = BOARD_LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    // One row of the rect, reused for every row.
    static uint8_t row[BOARD_LCD_WIDTH * 2];
    for (int i = 0; i < w; i++) {
        put_px(&row[i * 2], colour);
    }

    lcd_set_window(x, y, w, h);
    for (int r = 0; r < h; r++) {
        lcd_push_px(row, (size_t)w);
    }
}

// ── Public geometry helpers ─────────────────────────────────────────────────
void st7735_fill_rect(int x, int y, int w, int h, uint16_t colour)
{
    if (!s_ready) return;
    board_spi_lock();
    fill_rect_locked(x, y, w, h, colour);
    board_spi_unlock();
}

void st7735_fill_screen(uint16_t colour)
{
    st7735_fill_rect(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, colour);
}

// ── 5x7 font ────────────────────────────────────────────────────────────────
// Classic Adafruit-GFX / TFT_eSPI glcdfont glyphs, printable ASCII 0x20..0x7E.
// Each glyph is 5 column bytes, bit 0 = top pixel row. Extracted from the
// reference firmware's vendored font so the dashboard looks identical; no
// Arduino headers are involved at compile time.
static const uint8_t FONT5X7[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },  // ' '
    { 0x00, 0x00, 0x5F, 0x00, 0x00 },  // '!'
    { 0x00, 0x07, 0x00, 0x07, 0x00 },  // '"'
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 },  // '#'
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 },  // '$'
    { 0x23, 0x13, 0x08, 0x64, 0x62 },  // '%'
    { 0x36, 0x49, 0x56, 0x20, 0x50 },  // '&'
    { 0x00, 0x08, 0x07, 0x03, 0x00 },  // '\''
    { 0x00, 0x1C, 0x22, 0x41, 0x00 },  // '('
    { 0x00, 0x41, 0x22, 0x1C, 0x00 },  // ')'
    { 0x2A, 0x1C, 0x7F, 0x1C, 0x2A },  // '*'
    { 0x08, 0x08, 0x3E, 0x08, 0x08 },  // '+'
    { 0x00, 0x80, 0x70, 0x30, 0x00 },  // ','
    { 0x08, 0x08, 0x08, 0x08, 0x08 },  // '-'
    { 0x00, 0x00, 0x60, 0x60, 0x00 },  // '.'
    { 0x20, 0x10, 0x08, 0x04, 0x02 },  // '/'
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },  // '0'
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },  // '1'
    { 0x72, 0x49, 0x49, 0x49, 0x46 },  // '2'
    { 0x21, 0x41, 0x49, 0x4D, 0x33 },  // '3'
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },  // '4'
    { 0x27, 0x45, 0x45, 0x45, 0x39 },  // '5'
    { 0x3C, 0x4A, 0x49, 0x49, 0x31 },  // '6'
    { 0x41, 0x21, 0x11, 0x09, 0x07 },  // '7'
    { 0x36, 0x49, 0x49, 0x49, 0x36 },  // '8'
    { 0x46, 0x49, 0x49, 0x29, 0x1E },  // '9'
    { 0x00, 0x00, 0x14, 0x00, 0x00 },  // ':'
    { 0x00, 0x40, 0x34, 0x00, 0x00 },  // ';'
    { 0x00, 0x08, 0x14, 0x22, 0x41 },  // '<'
    { 0x14, 0x14, 0x14, 0x14, 0x14 },  // '='
    { 0x00, 0x41, 0x22, 0x14, 0x08 },  // '>'
    { 0x02, 0x01, 0x59, 0x09, 0x06 },  // '?'
    { 0x3E, 0x41, 0x5D, 0x59, 0x4E },  // '@'
    { 0x7C, 0x12, 0x11, 0x12, 0x7C },  // 'A'
    { 0x7F, 0x49, 0x49, 0x49, 0x36 },  // 'B'
    { 0x3E, 0x41, 0x41, 0x41, 0x22 },  // 'C'
    { 0x7F, 0x41, 0x41, 0x41, 0x3E },  // 'D'
    { 0x7F, 0x49, 0x49, 0x49, 0x41 },  // 'E'
    { 0x7F, 0x09, 0x09, 0x09, 0x01 },  // 'F'
    { 0x3E, 0x41, 0x41, 0x51, 0x73 },  // 'G'
    { 0x7F, 0x08, 0x08, 0x08, 0x7F },  // 'H'
    { 0x00, 0x41, 0x7F, 0x41, 0x00 },  // 'I'
    { 0x20, 0x40, 0x41, 0x3F, 0x01 },  // 'J'
    { 0x7F, 0x08, 0x14, 0x22, 0x41 },  // 'K'
    { 0x7F, 0x40, 0x40, 0x40, 0x40 },  // 'L'
    { 0x7F, 0x02, 0x1C, 0x02, 0x7F },  // 'M'
    { 0x7F, 0x04, 0x08, 0x10, 0x7F },  // 'N'
    { 0x3E, 0x41, 0x41, 0x41, 0x3E },  // 'O'
    { 0x7F, 0x09, 0x09, 0x09, 0x06 },  // 'P'
    { 0x3E, 0x41, 0x51, 0x21, 0x5E },  // 'Q'
    { 0x7F, 0x09, 0x19, 0x29, 0x46 },  // 'R'
    { 0x26, 0x49, 0x49, 0x49, 0x32 },  // 'S'
    { 0x03, 0x01, 0x7F, 0x01, 0x03 },  // 'T'
    { 0x3F, 0x40, 0x40, 0x40, 0x3F },  // 'U'
    { 0x1F, 0x20, 0x40, 0x20, 0x1F },  // 'V'
    { 0x3F, 0x40, 0x38, 0x40, 0x3F },  // 'W'
    { 0x63, 0x14, 0x08, 0x14, 0x63 },  // 'X'
    { 0x03, 0x04, 0x78, 0x04, 0x03 },  // 'Y'
    { 0x61, 0x59, 0x49, 0x4D, 0x43 },  // 'Z'
    { 0x00, 0x7F, 0x41, 0x41, 0x41 },  // '['
    { 0x02, 0x04, 0x08, 0x10, 0x20 },  // '\'
    { 0x00, 0x41, 0x41, 0x41, 0x7F },  // ']'
    { 0x04, 0x02, 0x01, 0x02, 0x04 },  // '^'
    { 0x40, 0x40, 0x40, 0x40, 0x40 },  // '_'
    { 0x00, 0x03, 0x07, 0x08, 0x00 },  // '`'
    { 0x20, 0x54, 0x54, 0x78, 0x40 },  // 'a'
    { 0x7F, 0x28, 0x44, 0x44, 0x38 },  // 'b'
    { 0x38, 0x44, 0x44, 0x44, 0x28 },  // 'c'
    { 0x38, 0x44, 0x44, 0x28, 0x7F },  // 'd'
    { 0x38, 0x54, 0x54, 0x54, 0x18 },  // 'e'
    { 0x00, 0x08, 0x7E, 0x09, 0x02 },  // 'f'
    { 0x18, 0xA4, 0xA4, 0x9C, 0x78 },  // 'g'
    { 0x7F, 0x08, 0x04, 0x04, 0x78 },  // 'h'
    { 0x00, 0x44, 0x7D, 0x40, 0x00 },  // 'i'
    { 0x20, 0x40, 0x40, 0x3D, 0x00 },  // 'j'
    { 0x7F, 0x10, 0x28, 0x44, 0x00 },  // 'k'
    { 0x00, 0x41, 0x7F, 0x40, 0x00 },  // 'l'
    { 0x7C, 0x04, 0x78, 0x04, 0x78 },  // 'm'
    { 0x7C, 0x08, 0x04, 0x04, 0x78 },  // 'n'
    { 0x38, 0x44, 0x44, 0x44, 0x38 },  // 'o'
    { 0xFC, 0x18, 0x24, 0x24, 0x18 },  // 'p'
    { 0x18, 0x24, 0x24, 0x18, 0xFC },  // 'q'
    { 0x7C, 0x08, 0x04, 0x04, 0x08 },  // 'r'
    { 0x48, 0x54, 0x54, 0x54, 0x24 },  // 's'
    { 0x04, 0x04, 0x3F, 0x44, 0x24 },  // 't'
    { 0x3C, 0x40, 0x40, 0x20, 0x7C },  // 'u'
    { 0x1C, 0x20, 0x40, 0x20, 0x1C },  // 'v'
    { 0x3C, 0x40, 0x30, 0x40, 0x3C },  // 'w'
    { 0x44, 0x28, 0x10, 0x28, 0x44 },  // 'x'
    { 0x4C, 0x90, 0x90, 0x90, 0x7C },  // 'y'
    { 0x44, 0x64, 0x54, 0x4C, 0x44 },  // 'z'
    { 0x00, 0x08, 0x36, 0x41, 0x00 },  // '{'
    { 0x00, 0x00, 0x77, 0x00, 0x00 },  // '|'
    { 0x00, 0x41, 0x36, 0x08, 0x00 },  // '}'
    { 0x02, 0x01, 0x02, 0x04, 0x02 },  // '~'
};

// Rendered-line scratch, byte-swapped and ready to push. Sized for the widest
// scaled line the dashboard can draw: 160 px * 8 rows at scale 1 (or 80 px *
// 16 rows at scale 2 - the same box). Static because only one painter exists
// and it holds the board mutex for the whole call; keeping it off the stack
// matters on a 4 KiB httpd task stack.
#define TEXTBUF_BYTES (BOARD_LCD_WIDTH * 16 * 2)
static uint8_t s_textbuf[TEXTBUF_BYTES];

// Renders one line into s_textbuf and pushes it at (x,y). Assumes the caller
// holds the board mutex. Returns after drawing nothing if the run is empty or
// entirely off-screen.
static void draw_string_locked(int x, int y, const char *s, uint16_t fg,
                               uint16_t bg, uint8_t fontScale)
{
    if (!s) return;
    if (fontScale < 1) fontScale = 1;

    const int cw = ST7735_CHAR_W * fontScale;
    const int chh = ST7735_CHAR_H * fontScale;

    // Start-of-line clipping: drop characters that fall entirely off the left.
    while (x < 0 && *s) { x += cw; s++; }
    if (!*s) return;

    int len = (int)strlen(s);
    int maxChars = (BOARD_LCD_WIDTH - x) / cw;
    if (maxChars <= 0) return;
    if (len > maxChars) len = maxChars;   // clip, never wrap

    int w = len * cw;
    int h = chh;
    int yStart = y;

    // Vertical clipping (rows near the bottom edge get cut, not scrolled).
    if (y < 0) { h += y; y = 0; }
    if (y + h > BOARD_LCD_HEIGHT) h = BOARD_LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    if ((size_t)w * (size_t)h * 2 > TEXTBUF_BYTES) return;

    const uint16_t fgCol = fg;
    const uint16_t bgCol = bg;

    for (int ci = 0; ci < len; ci++) {
        uint8_t c = (uint8_t)s[ci];
        if (c < 0x20 || c > 0x7E) c = ' ';
        const uint8_t *glyph = FONT5X7[c - 0x20];

        for (int gy = 0; gy < ST7735_CHAR_H; gy++) {
            int sy = yStart + gy * fontScale;
            if (sy < y || sy >= y + h) continue;
            int rowBase = (sy - y);
            for (int gx = 0; gx < ST7735_CHAR_W; gx++) {
                // Column 5 is the 1 px inter-character gap: always background.
                bool on = false;
                if (gx < 5 && gy < 7) {
                    on = (glyph[gx] >> gy) & 0x1;
                }
                uint16_t col = on ? fgCol : bgCol;
                for (int sx = 0; sx < fontScale; sx++) {
                    int px = ci * cw + gx * fontScale + sx;
                    if (px < 0 || px >= w) continue;
                    size_t off = ((size_t)rowBase * (size_t)w + (size_t)px) * 2;
                    put_px(&s_textbuf[off], col);
                }
            }
        }
    }

    lcd_set_window(x, y, w, h);
    lcd_push_px(s_textbuf, (size_t)w * (size_t)h);
}

void st7735_draw_string(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                        uint8_t fontScale)
{
    if (!s_ready || !s) return;
    board_spi_lock();
    draw_string_locked(x, y, s, fg, bg, fontScale);
    board_spi_unlock();
}

void st7735_draw_field(int x, int y, int w, int h, const char *s, uint16_t fg,
                       uint16_t bg, uint8_t fontScale)
{
    if (!s_ready) return;
    // One lock for the clear + the text: the dashboard must never leave a row
    // half-erased because an SD transfer landed between the two.
    board_spi_lock();
    fill_rect_locked(x, y, w, h, bg);
    draw_string_locked(x, y, s, fg, bg, fontScale);
    board_spi_unlock();
}

void st7735_fill_circle(int cx, int cy, int r, uint16_t colour)
{
    if (!s_ready || r <= 0) return;
    board_spi_lock();
    // Scanline fill: for each row inside the radius, the horizontal extent is
    // sqrt(r^2 - dy^2). Integer-only (no libm on the panel path) and plenty
    // accurate for an 8 px status dot.
    for (int dy = -r; dy <= r; dy++) {
        int dx2 = r * r - dy * dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= dx2) dx++;
        fill_rect_locked(cx - dx, cy + dy, dx * 2 + 1, 1, colour);
    }
    board_spi_unlock();
}

void st7735_batch_begin(void) { board_spi_lock(); }
void st7735_batch_end(void) { board_spi_unlock(); }

// ── Backlight ───────────────────────────────────────────────────────────────
// Active LOW, so duty 255 = fully off and duty (255 - level) = level/255 bright.
static void backlight_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BOARD_LCD_BL_LEDC_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight timer failed (%s), forcing BL on",
                 esp_err_to_name(err));
        gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_LCD_BL,
                             .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(BOARD_LCD_BL, 0);   // active low = on
        return;
    }

    ledc_channel_config_t ccfg = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BOARD_LCD_BL_LEDC_CH,
        .timer_sel = LEDC_TIMER_0,
        .duty = (uint32_t)(255 - BOARD_LCD_BL_LEVEL),
        .hpoint = 0,
    };
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        ESP_LOGW(TAG, "backlight channel failed, forcing BL on");
        gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_LCD_BL,
                             .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(BOARD_LCD_BL, 0);
    }
}

// ── Init ────────────────────────────────────────────────────────────────────
static void lcd_hw_reset(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_LCD_RST) | (1ULL << BOARD_LCD_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);

    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

// Standard ST7735R init, matching TFT_eSPI's Rcmd1/Rcmd2green/Rcmd3 for the
// green-tab panel. BGR colour order throughout (MAD_BGR).
static void lcd_init_sequence(void)
{
    lcd_cmd(CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(CMD_FRMCTR1);
    { const uint8_t d[] = { 0x01, 0x2C, 0x2D }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_FRMCTR2);
    { const uint8_t d[] = { 0x01, 0x2C, 0x2D }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_FRMCTR3);
    { const uint8_t d[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D };
      lcd_data(d, sizeof(d)); }

    lcd_cmd(CMD_INVCTR);
    lcd_data1(0x07);
    lcd_cmd(CMD_PWCTR1);
    { const uint8_t d[] = { 0xA2, 0x02, 0x84 }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_PWCTR2);
    lcd_data1(0xC5);
    lcd_cmd(CMD_PWCTR3);
    { const uint8_t d[] = { 0x0A, 0x00 }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_PWCTR4);
    { const uint8_t d[] = { 0x8A, 0x2A }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_PWCTR5);
    { const uint8_t d[] = { 0x8A, 0xEE }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_VMCTR1);
    lcd_data1(0x0E);

    lcd_cmd(CMD_INVOFF);
    lcd_cmd(CMD_MADCTL);
    lcd_data1(0xC0 | MAD_BGR);   // TFT_eSPI Rcmd1 value
    lcd_cmd(CMD_COLMOD);
    lcd_data1(0x05);             // 16-bit RGB565

    // Green-tab geometry: CGRAM origin (0,1) with XEND 0x81 / YEND 0xA0.
    lcd_cmd(CMD_CASET);
    { const uint8_t d[] = { 0x00, 0x02, 0x00, 0x81 }; lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_RASET);
    { const uint8_t d[] = { 0x00, 0x01, 0x00, 0xA0 }; lcd_data(d, sizeof(d)); }

    // This panel is an IPS variant: inversion ON is required (it is a
    // panel-batch property, not a bug - lily sets TFT_INVERSION_ON=1).
    lcd_cmd(CMD_INVON);

    lcd_cmd(CMD_GMCTRP1);
    { const uint8_t d[] = { 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2d,
                            0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10 };
      lcd_data(d, sizeof(d)); }
    lcd_cmd(CMD_GMCTRN1);
    { const uint8_t d[] = { 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                            0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10 };
      lcd_data(d, sizeof(d)); }

    lcd_cmd(CMD_NORON);
    vTaskDelay(pdMS_TO_TICKS(10));
    lcd_cmd(CMD_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Landscape (rotation 3) for the GREENTAB160x80 panel.
    lcd_cmd(CMD_MADCTL);
    lcd_data1(LCD_MADCTL_ROT3);
}

esp_err_t st7735_init(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = board_spi_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared SPI bus unavailable (%s)", esp_err_to_name(err));
        return err;
    }

    // Backlight off while initialising, so the panel never flashes garbage.
    // (Active LOW: drive it high = off until the first frame is painted.)
    gpio_config_t bl = { .pin_bit_mask = 1ULL << BOARD_LCD_BL,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_BL, 1);

    lcd_hw_reset();

    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = BOARD_LCD_SPI_HZ,
        .spics_io_num = BOARD_LCD_CS,
        .queue_size = 4,
    };
    err = board_spi_add_device(&dev, &s_lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD device add failed: %s", esp_err_to_name(err));
        return err;
    }

    board_spi_lock();
    lcd_init_sequence();
    board_spi_unlock();

    s_ready = true;
    backlight_init();
    ESP_LOGI(TAG, "ST7735 up: %dx%d landscape, cs=%d dc=%d",
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, BOARD_LCD_CS, BOARD_LCD_DC);

    // Painting a full black frame both clears whatever the panel powered up
    // with and gives the backlight something sane to reveal.
    st7735_fill_screen(ST7735_BLACK);
    return ESP_OK;
}

bool st7735_ready(void) { return s_ready; }
