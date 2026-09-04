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
// ── Status LED pin assignment — RESOLVED on hardware 2026-08-30 ──────────────
// The APA102 status LED is on GPIO2 (data) / GPIO6 (clock) — the SAME two pins
// as the LCD/SD SPI bus (MOSI=2, SCK=6) — NOT GPIO4/5 as the vendor's own
// pin_config.h (LED_DI=5, LED_CI=4) claims. The vendor pinout is WRONG for the
// LED on this board. GPIO4/5 are the chip's JTAG MTCK/MTDO pads; driving them
// never reaches the LED, which is why ~15 sessions of tests all saw a frozen
// "solid white/amber/cyan" — that was the LED holding stale data left on the
// shared bus, never responding. Proven with an on-hardware A/B pin test
// (firmware/src/diag/led_pinmap_diag.cpp): colours cycle correctly when driven
// on 2/6, stay frozen when driven on 5/4. This OVERTURNS the earlier "dead LED
// / hardware fault" conclusion — the hardware was healthy the whole time; it
// was a pin bug inherited from the vendor's pin_config.h.
// Sources: github.com/zombodotcom/T-Dongle-C5 (community examples repo that
// documents the 2/6 shared-bus wiring and the SPI-drive requirement) + the
// on-hardware test. See docs/knowledge-base/open-questions.md #1 and
// peripherals-apa102-led.md.
//
// Because the LED shares the LCD/SD SPI bus, it is driven over hardware SPI
// (CFG_LED_SHARED_SPI, see hal.cpp), NOT bit-banged, and re-latched after any
// LCD/SD bus traffic (Hal::ledRefresh) so the shared bus doesn't leave it
// showing garbage.
//
// usb_jtag_bridge_en is now MOOT for the LED (the LED isn't on the JTAG pins
// at all). Kept as a dead, 0 macro so the historical reasoning stays in one
// place; do NOT set it — per usb_serial_jtag_struct.h conf0 bit 15 it forces
// GPIO5 to input, and GPIO5 isn't the LED anyway. See open-questions.md #1.
#define CFG_RELEASE_JTAG_LED_PINS 0
#define CFG_LED_SHARED_SPI 1
#else
#define CFG_HAS_USB_HID  1
#define CFG_BOARD_NAME   "T-Dongle-S3"
#define CFG_MCU_NAME     "ESP32-S3"
#define CFG_RELEASE_JTAG_LED_PINS 0
// S3's APA102 is on its own dedicated pins (40/39), not the LCD bus — keep the
// bit-bang driver path.
#define CFG_LED_SHARED_SPI 0
#endif

// ── Hardware pin assignments ────────────────────────────────────────────────
#if defined(TARGET_DONGLE_C5)
// Source: LilyGO T-Dongle-C5 factory firmware pin_config.h
// LCD and SD share one SPI bus (distinct CS lines).

// ──── RGB LED (APA102, colour order BGR) — on the SHARED LCD/SD SPI bus,
//      NOT the JTAG pads the vendor pin_config.h lists. data=MOSI=2,
//      clock=SCK=6. Proven on hardware 2026-08-30; see the capability block
//      above and docs/knowledge-base/open-questions.md #1.
#define PIN_LED_DATA  2    // DI = PIN_LCD_MOSI (shared SPI bus)
#define PIN_LED_CLOCK 6    // CI = PIN_LCD_SCK  (shared SPI bus)

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
// Backlight level 0..255 (PWM; active-low on the C5, converted in display.cpp).
// Was forced to 0 (OFF) on the theory that backlight bleed washed the status
// LED "white" through the housing — but that "white" was the pin bug (the LED
// was on GPIO2/6 and never driven; see the LED block above and
// open-questions.md #1), not backlight bleed. With the LED actually working,
// the dashboard is restored. 180 is clearly readable; drop it if the LED reads
// washed on your unit.
#define CFG_LCD_BL_LEVEL 180

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

// Diagnostic toggle (off by default, T-Dongle-C5-5g env sets it to 1): move
// the SoftAP itself from 2.4GHz (CFG_WIFI_CHANNEL) to 5GHz channel 36
// (U-NII-1, non-DFS — no radar-detection delay, allowed in essentially every
// regulatory domain). This is separate from Module B's recon capture, which
// stays pinned to whatever channel the AP is actually on either way. Exists
// to test whether large-frame/streamed-download failures on this SoftAP are
// 2.4GHz airtime/congestion or a general (band-independent) limitation —
// C5 is WiFi 6 dual-band (SOC_WIFI_SUPPORT_5G=1 in the vendored sdkconfig),
// confirmed before writing this. NOT the default: this changes the C2
// server's actual operating band, a bigger change than anything else in
// Module B, so it's opt-in via build flag rather than silently on.
#ifndef CFG_WIFI_BAND_5G
#define CFG_WIFI_BAND_5G    0
#endif
#define CFG_WIFI_5G_CHANNEL 36

// On-device end-to-end test of the C2 REST API over loopback (no external WiFi
// client). Off by default; the T-Dongle-C5-selftest env sets it to 1. See
// firmware/src/c2/c2_selftest.cpp. Guard with #ifndef so a -D build flag wins.
#ifndef CFG_C2_SELFTEST
#define CFG_C2_SELFTEST     0
#endif

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

// ── WiFi Recon / PCAP capture (Module B, Phase 1) ───────────────────────────
// Phase 1 scope: promiscuous capture of mgmt + EAPOL data frames on the C2
// SoftAP's own channel, PCAP to SD, pulled over the REST API. Single radio —
// the SoftAP pins the channel, so this deliberately does NOT hop channels
// (that's Phase 2, and it would disturb the C2 server while it runs).
#define CFG_RECON_SNAPLEN     400   // bytes captured per frame (radiotap+80211hdr; covers mgmt/EAPOL)
#define CFG_RECON_RING_SLOTS  32    // ring buffer depth between the promiscuous RX callback and tick()
#define SD_RECON_DIR          "/recon"

#endif // LILY_DUCKY_CONFIG_H
