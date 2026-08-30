// =============================================================================
// led_spi_diag.cpp — standalone APA102 hardware-SPI diagnostic (T-Dongle-C5)
// =============================================================================
// Purpose: docs/knowledge-base/open-questions.md #1 (LED intermittency) still
// needs a genuinely hardware-timed driver tried, as opposed to digitalWrite
// bit-banging (both ours and the vendor's froze most of the time on two
// physical units) and the SPIClass(HSPI) attempt that silently no-op'd.
//
// That SPIClass(HSPI) failure is now root-caused, not just worked around:
// arduino-esp32's esp32-hal-spi.c sizes its bus table with SPI_COUNT, which
// on ESP32-C2/C3/C5/C6/C61/H2 (one general-purpose SPI peripheral each) is 1
// — only bus index 0 is valid. `HSPI` is `#define`d to 1 on these targets
// (cores/esp32-hal-spi.h; the comment there attributes HSPI to "ESP32S2, S3,
// P4 - SPI 3 bus", i.e. chips with a *second* GP-SPI peripheral, which the C5
// is not one of — confirmed via esp-idf's esp32c5/soc_caps.h, SOC_SPI_PERIPH_NUM=2,
// counting SPI0/SPI1 flash-shared plus one GP SPI2, not a second one). Passing
// spi_num=1 into spiStartBus() fails the `spi_num >= SPI_COUNT` bounds check,
// which logs at esp_log's default-suppressed level and returns NULL — silent
// from the sketch's point of view. `FSPI` (=0) is the only valid bus on this
// chip. See docs/knowledge-base/open-questions.md #1 and hardware-esp32-c5.md
// for the full citation trail.
//
// Also worth correcting here: earlier notes (config.h, AGENTS.md,
// open-questions.md) named "ESP-IDF's RMT-backed led_strip driver" as the next
// thing to try. That's the wrong backend for this LED — Espressif's own
// `led_strip` component docs state APA102/SK9822 use the *SPI* backend; RMT
// is for single-wire, timing-critical protocols (WS2812/NeoPixel-style), which
// the two-wire clock+data APA102 is not. This diagnostic uses hardware SPI
// directly (equivalent to what led_strip's SPI backend does under the hood)
// rather than pulling in the component for one LED.
//
// 2026-08-30, second finding: the first hardware-SPI run of this diagnostic
// (bridge_en=1, matching main firmware) produced a solid, unchanging WHITE
// LED — not the "clean colour cycling" a correct driver should show, and
// notably *not* intermittent the way the bit-banged tests were (see
// open-questions.md #1). Re-reading the full usb_serial_jtag_struct.h conf0
// register doc (not just the excerpt previously quoted) says, verbatim:
// "Set this bit usb_jtag, the connection between usb_jtag and internal JTAG
// is disconnected, and MTMS, MTDI, MTCK output via GPIO Matrix, MTDO inputs
// via GPIO Matrix." GPIO5 (MTDO) is PIN_LED_DATA. Setting bridge_en=1 forces
// that specific pin into INPUT direction at the peripheral level — software
// (digitalWrite, SPI MOSI, anything) cannot drive it as output while this bit
// is set, regardless of pinMode()/SPI.begin(). GPIO4 (MTCK/PIN_LED_CLOCK)
// stays an output either way, so the clock keeps toggling while data is
// stuck floating/pulled — a genuine, deterministic mechanism for "clock
// runs, data doesn't," which reads as solid/near-solid white on an APA102.
// The C5's own "Configure Other JTAG Interfaces" guide additionally states
// JTAG is connected to GPIO2-5 only when you ask for it (this bit, or an
// eFuse) — NOT by default — which is the missing piece explaining why the
// vendor's firmware (never touches this register) drives the same pins as
// plain GPIO with no conflict at all. LEDDIAG_SET_BRIDGE_EN (default 0,
// override via build flag) now controls whether this diagnostic pokes the
// register, so the corrected (0/default) and previous (1) behavior can both
// be re-tested and compared without editing this file.
//
// Build/flash/watch (does not touch the main firmware or its build env):
//   pio run -e T-Dongle-C5-leddiag -t upload -t monitor          (bridge_en=0, corrected theory)
//   pio run -e T-Dongle-C5-leddiag-bridge-en -t upload -t monitor (bridge_en=1, reproduces the white result)
//
// Confirm the user is actively watching the LED ("say go") before triggering
// any run of this — there is no way to observe the LED's actual state
// otherwise. Cycles RED/GREEN/BLUE/OFF/AMBER, 4s each, repeating, logging
// every transition over serial so a serial capture alone tells you what
// *should* be showing even without a simultaneous visual report.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include "config.h"

// Override point for this specific test — deliberately independent of
// CFG_RELEASE_JTAG_LED_PINS (config.h) so both hypotheses stay testable from
// one file without touching the main firmware's setting. Defaults to NOT
// setting the bridge — see the corrected theory in the file header comment.
#ifndef LEDDIAG_SET_BRIDGE_EN
#define LEDDIAG_SET_BRIDGE_EN 0
#endif

#if LEDDIAG_SET_BRIDGE_EN
#include <soc/usb_serial_jtag_struct.h>
#endif

static SPIClass ledSPI(FSPI);

// APA102 frame over hardware SPI: 4 zero bytes (start frame) + one LED frame
// (header | brightness, B, G, R) + 4 zero bytes (end frame — not the
// datasheet's 0xFFFFFFFF; see peripherals-apa102-led.md for why zeros are the
// portable choice). Sent as one buffer so SPI drives clock+data together with
// no CPU-loop jitter between bits, unlike digitalWrite bit-banging.
static void sendFrame(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
    uint8_t buf[4 + 4 + 4] = {0};
    buf[4] = 0xE0 | (brightness & 0x1F);
    buf[5] = b;
    buf[6] = g;
    buf[7] = r;
    ledSPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    ledSPI.writeBytes(buf, sizeof(buf));
    ledSPI.endTransaction();
}

struct ColorStep { const char* name; uint8_t r, g, b; };
static const ColorStep STEPS[] = {
    {"RED",   255, 0,   0},
    {"GREEN", 0,   255, 0},
    {"BLUE",  0,   0,   255},
    {"OFF",   0,   0,   0},
    {"AMBER", 255, 191, 0},
};
static const uint8_t STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
static const uint32_t HOLD_MS = 4000;
static const uint8_t BRIGHTNESS = 20;   // deliberately bright — this is a visibility test, not the production D2 recipe

void setup() {
    Serial.begin(115200);
    uint32_t waitStart = millis();
    while (!Serial && millis() - waitStart < 8000) delay(10);
    delay(300);

    Serial.println();
    Serial.println("[LEDDIAG] APA102 hardware-SPI (FSPI) diagnostic — T-Dongle-C5");
    Serial.printf("[LEDDIAG] PIN_LED_CLOCK(SCK)=%d PIN_LED_DATA(MOSI)=%d\n",
                  PIN_LED_CLOCK, PIN_LED_DATA);

#if LEDDIAG_SET_BRIDGE_EN
    USB_SERIAL_JTAG.conf0.usb_jtag_bridge_en = 1;
    Serial.println("[LEDDIAG] usb_jtag_bridge_en = 1 (per conf0 doc: forces MTDO/GPIO5/DATA to INPUT — expect this to fail)");
#else
    Serial.println("[LEDDIAG] usb_jtag_bridge_en left at default 0 (corrected theory: pins are plain GPIO by default, register untouched)");
#endif

    // MISO/SS unused — APA102 is clock+data only, no chip-select, no return path.
    ledSPI.begin(PIN_LED_CLOCK, -1, PIN_LED_DATA, -1);
    Serial.println("[LEDDIAG] SPIClass(FSPI).begin() done — waiting for \"go\" before first frame is implicit: watch now.");
    Serial.println("[LEDDIAG] Sequence: RED -> GREEN -> BLUE -> OFF -> AMBER, 4s each, repeating. Ctrl+C on the monitor to stop.");
}

// Falsification test: does the LED ever go fully dark under a sustained,
// unambiguous all-zero stream? If not, no amount of framing/protocol/timing
// tuning on our side can matter — the data line isn't reaching the LED, or
// the LED can't respond to it. Not a proper APA102 frame at all, deliberately
// — just raw zero bytes, continuously, so there's no ambiguity about framing.
static void sendRaw(uint8_t value, uint32_t durationMs) {
    uint8_t buf[64];
    memset(buf, value, sizeof(buf));
    ledSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    uint32_t start = millis();
    while (millis() - start < durationMs) {
        ledSPI.writeBytes(buf, sizeof(buf));
    }
    ledSPI.endTransaction();
}

void loop() {
    static uint32_t cycle = 0;

    Serial.println("[LEDDIAG] RAW TEST: continuous all-ZERO bytes for 5s (proper APA102 framing not used here) — LED should go fully dark if the data line does anything at all");
    sendRaw(0x00, 5000);

    Serial.println("[LEDDIAG] RAW TEST: continuous all-ONE (0xFF) bytes for 5s — sanity check the raw path can also drive it");
    sendRaw(0xFF, 5000);

    for (uint8_t i = 0; i < STEP_COUNT; i++) {
        const ColorStep& s = STEPS[i];
        Serial.printf("[LEDDIAG] cycle=%lu step=%u -> %s (r=%u g=%u b=%u)\n",
                      (unsigned long)cycle, i, s.name, s.r, s.g, s.b);
        sendFrame(s.r, s.g, s.b, BRIGHTNESS);
        delay(HOLD_MS);
    }
    cycle++;
}
