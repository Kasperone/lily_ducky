// =============================================================================
// config.h — pinouts, capabilities, and compile-time settings
// =============================================================================
// One config file, two targets. The build system defines exactly one of
// TARGET_DONGLE_C5 / TARGET_DONGLE_S3 (see platformio.ini); every pin and
// capability difference between the boards resolves here so no other file
// needs target-specific #ifs.
//
//   T-Dongle-C5  (ESP32-C5)  — primary target, hardware on hand.
//      NO USB-OTG peripheral: the USB-A port is a fixed-function
//      Serial/JTAG console only. The device cannot enumerate as a
//      keyboard. It runs the C2 dashboard, interpreter, LCD, and SD.
//   T-Dongle-S3  (ESP32-S3)  — full BadUSB target (USB HID keyboard).
// =============================================================================
#ifndef LILY_DUCKY_CONFIG_H
#define LILY_DUCKY_CONFIG_H

#if !defined(TARGET_DONGLE_C5) && !defined(TARGET_DONGLE_S3)
#error "Define TARGET_DONGLE_C5 or TARGET_DONGLE_S3 (set via platformio.ini build_flags)"
#endif

// ── Hardware capabilities ───────────────────────────────────────────────────
// USB HID needs the USB-OTG peripheral. ESP32-C5 has none (only the
// fixed-function USB Serial/JTAG controller), so every HID feature —
// keystroke injection, OS detect, exfil capture, MSC, VID/PID spoofing —
// compiles to honest no-ops on the C5 build.
#if defined(TARGET_DONGLE_C5)
#define CFG_HAS_USB_HID  0
#define CFG_BOARD_NAME   "T-Dongle-C5"
#define CFG_MCU_NAME     "ESP32-C5"
// The board's APA102 is wired to the chip's MTCK/MTDO pads (GPIO4/5 — clock/
// data respectively; datasheet strapping table, not MTDI as this comment used
// to say), which double as the USB Serial/JTAG controller's JTAG pins.
//
// CORRECTED 2026-08-30 — this bit's effect was backwards from what this
// comment used to claim, confirmed against the register's own full
// documentation text (usb_serial_jtag_struct.h conf0, bit 15): setting it
// makes MTMS/MTDI/MTCK GPIO2-4 outputs but makes **MTDO/GPIO5 an input** —
// GPIO5 is PIN_LED_DATA. Setting this bit does not "release" the LED pins,
// it forces the data pin specifically into a direction the LED driver can
// never write to, regardless of digitalWrite/SPIClass/anything else. The
// C5's own "Configure Other JTAG Interfaces" guide additionally confirms
// JTAG is NOT wired to GPIO2-5 by default at all — it's only bridged there
// when this bit (or an eFuse) explicitly asks for it — so there was never
// anything to "release" in the first place. LilyGO's own factory firmware
// never touches this register and drives GPIO4/5 as plain GPIO successfully.
// This macro should be 0; it is still 1 only because flipping it needs a
// hardware pass to verify nothing else regresses (see
// docs/knowledge-base/open-questions.md #1, 2026-08-30 addendum, which
// retested with the bit left untouched and confirms it made no difference
// for this specific unit's LED — a separate, still-open hardware question).
//
// LED status: NOT resolved as a firmware bug, and NOT flatly "confirmed
// dead" either — two earlier "confirmed dead" conclusions in this project
// were retracted. A 2026-08-30 session used a genuinely hardware-timed
// driver (SPIClass(FSPI) — NOT RMT; Espressif's own led_strip component
// docs say APA102/SK9822 use the SPI backend, RMT is for single-wire
// protocols) and, separately, both bridge_en states, and still got a
// solid, unresponsive white — including under a raw all-zero data stream, a
// healthy LED cannot show that. Read open-questions.md #1 in full — including
// what that evidence does and doesn't prove — before changing this line or
// sendAPA102() again.
//
// Flipped 0 2026-08-30: verified on hardware (both in the diag build and in
// this actual main-firmware build — full boot sequence re-captured over
// serial: SD mount, LCD init, WiFi C2 SoftAP start all unaffected) that not
// setting this bit changes nothing else. Kept as a named macro rather than
// deleted outright so the history/reasoning stays attached to one place.
#define CFG_RELEASE_JTAG_LED_PINS 0
#else
#define CFG_HAS_USB_HID  1
#define CFG_BOARD_NAME   "T-Dongle-S3"
#define CFG_MCU_NAME     "ESP32-S3"
#define CFG_RELEASE_JTAG_LED_PINS 0
#endif

// ── Hardware pin assignments ────────────────────────────────────────────────
#if defined(TARGET_DONGLE_C5)
// Source: LilyGO T-Dongle-C5 factory firmware pin_config.h
// LCD and SD share one SPI bus (distinct CS lines).

// ──── RGB LED (APA102, colour order BGR)
#define PIN_LED_DATA  5    // DI
#define PIN_LED_CLOCK 4    // CI

// ──── Button (BOOT key; holding at plug-in enters download mode)
#define PIN_BUTTON    28   // active LOW

// ──── SD card (SPI mode — the C5 wires CMD/DAT0 as SPI MOSI/MISO)
#define PIN_SD_CS     23
#define SD_USE_SPI    1

// ──── LCD (ST7735 0.96" 80x160 IPS, shared SPI bus)
#define PIN_LCD_MOSI  2
#define PIN_LCD_MISO  7
#define PIN_LCD_SCK   6
#define PIN_LCD_CS    10
#define PIN_LCD_DC    3
#define PIN_LCD_RST   1
#define PIN_LCD_BL    0
// Backlight level 0..255 (PWM). Full brightness washes the status LED out
// to "white" through the transparent housing (verified on hardware); dimmed
// keeps the LCD readable and the LED visible.
#define CFG_LCD_BL_LEVEL 0     // D2 recipe: backlight OFF so LED is unambiguous

#else // TARGET_DONGLE_S3

// ──── RGB LED (APA102, colour order BGR)
#define PIN_LED_DATA  40   // DIN
#define PIN_LED_CLOCK 39   // CLK

// ──── Button
#define PIN_BUTTON    0    // GPIO0, active LOW

// ──── SD card (SDMMC 1-bit mode — uses SD_MMC library)
#define PIN_SDMMC_CMD 16
#define PIN_SDMMC_CLK 12
#define PIN_SDMMC_D0  14
// 1-bit mode: only D0 used, D1/D2 don't need to be wired
#define SDMMC_USE_1BIT 1

// ──── LCD (ST7735 0.96" 80x160 IPS, SPI) — official T-Dongle-S3 pins,
//      cross-checked against LilyGO's MicroPython tft_config.py.
//      Display panel: BGR colour order, inverted (set in TFT_eSPI build flags).
#define PIN_LCD_MOSI  3
#define PIN_LCD_SCK   5
#define PIN_LCD_CS    4
#define PIN_LCD_DC    2
#define PIN_LCD_RST   1
#define PIN_LCD_BL    37   // backlight; LilyGO official; if your board is dark, try 38
#define CFG_LCD_BL_LEVEL 255  // untested on hardware; full brightness until proven otherwise
#endif

// ──── LCD panel (identical on both boards)
#define LCD_WIDTH     80
#define LCD_HEIGHT    160
#define LCD_ENABLED   1    // set to 0 to compile without LCD support

// ── WiFi C2 defaults ────────────────────────────────────────────────────────

#define CFG_WIFI_SSID       "LilyC2"
#define CFG_WIFI_PASS       "quackquack"
#define CFG_WIFI_CHANNEL    6
#define CFG_AP_IP           "192.168.4.1"
#define CFG_HTTP_PORT       80

// ── DuckyScript interpreter defaults ────────────────────────────────────────

#define CFG_DEFAULT_DELAY     200     // ms between commands (pre-Duckyscript v2)
#define CFG_MIN_DELAY         10      // absolute minimum DELAY value
#define CFG_MAX_VAR           65535   // uint16 cap for DuckvScript variables
#define CFG_MAX_FUNCTIONS     32      // max named functions in symbol table
#define CFG_MAX_VAR_COUNT     32      // max $variables at once
#define CFG_MAX_NESTED        16      // max nested block depth (IF/WHILE/FUNC)
#define CFG_PAYLOAD_FILENAME  "payload.dd"  // default filename on SD card
#define CFG_MAX_PAYLOAD_FN    48      // max chars in a payload filename

// ── USB VID / PID — impersonate a trusted device (S3 only) ─────────────────
// Spoofed to look like a generic Logitech keyboard/receiver. Baked into the
// TinyUSB descriptor at compile time; the C5's Serial/JTAG controller has a
// fixed Espressif identity that cannot be changed.
#define CFG_USB_VID           0x046D  // Logitech
#define CFG_USB_PID           0xC52B  // Unifying Receiver-style
#define CFG_USB_MFR           "Logitech"
#define CFG_USB_PROD          "USB Receiver"
#define CFG_USB_SERIAL        "C52B-00000000"

// Alternate VID/PID presets (swap in config or via ATTACKMODE)
#define USB_VID_DELL          0x413C
#define USB_PID_DELL_KB216    0x2113
#define USB_VID_HP            0x03F0
#define USB_PID_HP_KB         0x034A

// ── SD card path constants ──────────────────────────────────────────────────

#define SD_PAYLOAD_DIR        "/payloads"
#define SD_LOOT_FILE          "/loot.bin"

// ── OS Detection (S3 only — needs HID LED reports from the host) ───────────
#define CFG_OS_DETECT_WINDOW_MS  3000  // ms to observe host SET_REPORT traffic

// OS ID constants used by $_OS built-in variable
#define OS_UNKNOWN   0
#define OS_WINDOWS   1
#define OS_MACOS     2
#define OS_LINUX     3
#define OS_CHROMEOS  4

// ── Keyboard Layouts ────────────────────────────────────────────────────────
#define LAYOUT_US    0
#define LAYOUT_PL    1  // Polish Programmer's (mostly US, AltGr for diacritics)
#define LAYOUT_DE    2  // German QWERTZ
#define LAYOUT_COUNT 3

// ── Exfiltration (S3 only — captures keystrokes the device itself types) ───
#define SD_LOOT_DIR           "/loot"
#define SD_LOOT_KEYLOG_FILE   "/loot/keylog.bin"
#define CFG_EXFIL_BUF_SIZE    4096  // keystroke capture buffer

// ── Boot behaviour ──────────────────────────────────────────────────────────

// Auto-fire the first payload on USB plug-in (after HID enumeration delay).
// Ignored on the C5: without USB HID there is nothing to type with.
#define CFG_AUTO_FIRE_ON_PLUG   1
#define CFG_ENUMERATION_DELAY_MS 2000  // give the host time to see the HID

// How long setup() waits for the USB console host to connect before printing
// the boot log (hardware-CDC targets only). Bytes printed before the host
// enumerates are dropped — including the C2 auth token, which is otherwise
// unrecoverable until the next reset. Timeout keeps a headless boot finite.
#define CFG_SERIAL_CONNECT_WAIT_MS 8000

#endif // LILY_DUCKY_CONFIG_H
