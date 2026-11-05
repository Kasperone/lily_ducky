// =============================================================================
// board_pins.h - T-Dongle-C5 pin map for the shared SPI peripherals (layer 3c)
// =============================================================================
// Only the constants the LCD / SD / APA102 actually need are copied here from
// lily_ducky's firmware/src/config.h (TARGET_DONGLE_C5 branch). The Arduino
// config.h itself is deliberately NOT imported: it drags in S3 pinouts,
// capability flags and USB/CDC tuning this tree has no use for.
//
// One physical SPI bus is shared by three devices, each with its own chip
// select - except the APA102, which has none:
//
//   SPI2_HOST   SCLK = GPIO6   MOSI = GPIO2   MISO = GPIO7
//     SD      CS = GPIO23
//     LCD     CS = GPIO10,  DC = GPIO3,  RST = GPIO1,  BL = GPIO0 (active LOW)
//     APA102  DI = GPIO2 (== MOSI),  CI = GPIO6 (== SCLK),  no CS
//
// The APA102's pins are NOT GPIO4/5. The vendor's pin_config.h (LED_DI=5,
// LED_CI=4) is wrong for this board; GPIO4/5 are the chip's JTAG MTCK/MTDO
// pads and driving them never reaches the LED. Proven on hardware 2026-08-30 -
// see lily's docs/knowledge-base/peripherals-apa102-led.md.
//
// Because the LED shares MOSI/SCK with no chip select, every LCD paint or SD
// transfer clocks data straight through it and leaves it showing garbage. That
// is why every bus user re-latches the LED afterwards (board_led_relatch()).
// =============================================================================
#ifndef C5_BOARD_PINS_H
#define C5_BOARD_PINS_H

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ── Shared bus ──────────────────────────────────────────────────────────────
#define BOARD_SPI_HOST        SPI2_HOST
#define BOARD_PIN_SCLK        GPIO_NUM_6
#define BOARD_PIN_MOSI        GPIO_NUM_2
#define BOARD_PIN_MISO        GPIO_NUM_7

// Largest single SPI transfer the bus accepts. LCD pixel pushes are chunked to
// stay under this; the bus is initialised with the same figure so nothing can
// queue a transfer the DMA descriptor cannot hold.
#define BOARD_SPI_MAX_TRANSFER 4096
// Chunk size for LCD pixel streams (a whole 160x80 frame is 25600 bytes, far
// over the bus limit, so full-screen fills are pushed in slices of this size).
#define BOARD_LCD_CHUNK_BYTES 2048

// ── SD card (SPI mode) ──────────────────────────────────────────────────────
#define BOARD_SD_CS           GPIO_NUM_23
#define BOARD_SD_MAX_FREQ_KHZ 20000   // SDSPI ceiling; the card negotiates down

// ── LCD (ST7735 0.96" 80x160 IPS, used landscape as 160x80) ─────────────────
#define BOARD_LCD_CS          GPIO_NUM_10
#define BOARD_LCD_DC          GPIO_NUM_3
#define BOARD_LCD_RST         GPIO_NUM_1
#define BOARD_LCD_BL          GPIO_NUM_0   // backlight, ACTIVE LOW

// Panel geometry in its native orientation, and the landscape frame the
// dashboard is painted in (rotation 3, same as lily's setRotation(3)).
#define BOARD_LCD_NATIVE_W    80
#define BOARD_LCD_NATIVE_H    160
#define BOARD_LCD_WIDTH       160
#define BOARD_LCD_HEIGHT      80

// lily runs the panel at 27 MHz (SPI_FREQUENCY in platformio.ini). 26 MHz is
// the same nominal divide off the 80 MHz SPI clock and leaves the SD card's
// 20 MHz ceiling untouched - they are separate spi_devices on one host.
#define BOARD_LCD_SPI_HZ      (26 * 1000 * 1000)

// Backlight PWM (LEDC). 0..255 brightness; active-low so the duty written to
// the pin is the inverse. lily uses 180 and it is clearly readable.
#define BOARD_LCD_BL_LEVEL    180
#define BOARD_LCD_BL_LEDC_CH  0
#define BOARD_LCD_BL_LEDC_HZ  5000

// ── APA102 status LED (shares MOSI/SCK, no chip select) ─────────────────────
#define BOARD_APA102_BRIGHTNESS 10    // vendor factory value (lily's hal.cpp)
#define BOARD_LED_SPI_HZ        (4 * 1000 * 1000)

// ── SoftAP ──────────────────────────────────────────────────────────────────
// Must match the channel main.c configures the SoftAP on: recon capture is
// pinned to it and scan/enum restore to it.
#define BOARD_AP_CHANNEL      6

#endif  // C5_BOARD_PINS_H
