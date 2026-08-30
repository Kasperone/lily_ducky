// =============================================================================
// led_pinmap_diag.cpp — decides WHICH pins the APA102 is actually on (C5)
// =============================================================================
// Why this exists: a genuine primary-source CONFLICT surfaced 2026-08-30 over
// the T-Dongle-C5's status-LED pin assignment — see
// docs/knowledge-base/open-questions.md #1 (the pin-map dispute) and #7.
//
//   * VENDOR (Xinyuan-LilyGO/T-Dongle-C5, include/pin_config.h + Factory.ino,
//     Pololu APA102 lib):   LED_DI=5 (data), LED_CI=4 (clock)  — GPIO4/5,
//     which are also the chip's JTAG MTCK/MTDO pads.
//   * THIRD-PARTY (github.com/zombodotcom/T-Dongle-C5, a community examples
//     repo with a detailed, hardware-specific shared-bus writeup):
//     LED DI=2 (data), CI=6 (clock) — the SAME pins as the LCD's MOSI/SCK,
//     i.e. the APA102 hangs off the display SPI bus.
//
// These cannot both be right (one LED = one data pin + one clock pin). The
// zombodotcom account also explains every symptom this project has ever seen
// on this LED: the vendor factory firmware drives the LCD on 2/6 while
// "driving the LED" on 4/5, so an LED actually wired to 2/6 would show the
// LCD's SPI traffic as garbage colour ("dim purple/pink with white flashing"
// — exactly what was observed); and every one of this project's own diags ran
// with the LCD OFF while driving 4/5, so an LED on 2/6 would see an idle bus
// and sit at a static colour (the "solid white" every prior test produced).
// The whole "dead LED" conclusion may simply be the wrong pins.
//
// This diagnostic settles it empirically in ONE flash. It drives a real
// APA102 colour cycle on BOTH candidate pin pairs, in turn, clearly narrated
// over serial, so whoever is watching the LED can see which pinset (if any)
// it responds to:
//
//   PHASE A — data=2  clock=6  (zombodotcom / shared-LCD-bus theory)  <-- the new hypothesis
//   PHASE B — data=5  clock=4  (vendor pin_config.h theory)           <-- the control; every prior test = solid white
//
// LED-only: the LCD/SD are NOT initialised, and the backlight (GPIO0,
// active-low) is forced OFF, so nothing contends for GPIO2/6 while PHASE A
// drives them. usb_jtag_bridge_en is left at its default 0 throughout
// (open-questions.md #1: setting it forces GPIO5 to INPUT — irrelevant to the
// 2/6 pins, and wrong for 4/5 anyway).
//
// Build/flash/watch (does NOT touch the main firmware or its build env):
//   pio run -e T-Dongle-C5-pinmap -t upload -t monitor
//
// HARD RULE (CLAUDE.md): confirm the user is actively watching the LED before
// any run of this — the LED's real state cannot be observed any other way.
// Each colour holds 3s; the serial log narrates every transition so a capture
// alone tells you what SHOULD be showing at any moment.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include "config.h"

// Candidate pin pairs. {data (MOSI), clock (SCK)}.
static const int PINS_A_DATA  = 2;   // zombodotcom: DI shared with LCD MOSI
static const int PINS_A_CLOCK = 6;   // zombodotcom: CI shared with LCD SCK
static const int PINS_B_DATA  = PIN_LED_DATA;   // vendor: 5 (MTDO)
static const int PINS_B_CLOCK = PIN_LED_CLOCK;  // vendor: 4 (MTCK)

static SPIClass ledSPI(FSPI);   // FSPI is the only valid GP-SPI bus on the C5

struct ColorStep { const char* name; uint8_t r, g, b; };
static const ColorStep STEPS[] = {
    {"RED",   255, 0,   0},
    {"GREEN", 0,   255, 0},
    {"BLUE",  0,   0,   255},
    {"OFF",   0,   0,   0},
    {"WHITE", 255, 255, 255},
};
static const uint8_t STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
static const uint32_t HOLD_MS = 3000;
static const uint8_t BRIGHTNESS = 24;   // bright, visible; not the 10 production recipe

// One APA102 frame: 4-byte zero start, LED frame (0xE0|brightness, B, G, R),
// 4-byte zero end (portable choice — see peripherals-apa102-led.md; genuine
// on real APA102, required on SK9822 clones). Sent as one buffer so hardware
// SPI clocks data out with no CPU-loop jitter.
static void sendFrame(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t buf[12] = {0};
    buf[4] = 0xE0 | (BRIGHTNESS & 0x1F);
    buf[5] = b;
    buf[6] = g;
    buf[7] = r;
    ledSPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
    ledSPI.writeBytes(buf, sizeof(buf));
    ledSPI.endTransaction();
}

static void runPhase(const char* label, int dataPin, int clockPin) {
    Serial.printf("\n[PINMAP] ===== PHASE %s : data(MOSI)=GPIO%d clock(SCK)=GPIO%d =====\n",
                  label, dataPin, clockPin);
    Serial.println("[PINMAP] watch the LED now — if colours appear on THIS phase, the LED is on THESE pins");
    ledSPI.begin(clockPin, -1, dataPin, -1);   // (sck, miso, mosi, ss); APA102 has no MISO/CS
    for (uint8_t i = 0; i < STEP_COUNT; i++) {
        const ColorStep& s = STEPS[i];
        Serial.printf("[PINMAP]   phase %s -> %-5s (r=%u g=%u b=%u)\n",
                      label, s.name, s.r, s.g, s.b);
        sendFrame(s.r, s.g, s.b);
        delay(HOLD_MS);
    }
    ledSPI.end();   // release the bus so the next phase can rebind FSPI to other pins
}

void setup() {
    Serial.begin(115200);
    uint32_t waitStart = millis();
    while (!Serial && millis() - waitStart < 8000) delay(10);
    delay(300);

    // LCD backlight OFF (active-low on the C5): drive GPIO0 HIGH. The LCD
    // itself is never initialised, so GPIO2/6 stay free for PHASE A.
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_LCD_BL, HIGH);

    Serial.println();
    Serial.println("[PINMAP] APA102 pin-map decider — T-Dongle-C5");
    Serial.println("[PINMAP] Resolves the vendor(4/5) vs zombodotcom(2/6) LED-pin conflict.");
    Serial.println("[PINMAP] PHASE A = 2/6 (new theory), PHASE B = 5/4 (vendor; every prior test = white).");
    Serial.println("[PINMAP] Colours cycle RED/GREEN/BLUE/OFF/WHITE, 3s each, per phase, repeating.");
}

// PINMAP_PHASE selects a SINGLE pinset to drive continuously, so a watcher
// only has to recognise "colours or not" without tracking which phase is which:
//   -DPINMAP_PHASE=1  -> only Phase A (data=2  clock=6, zombodotcom theory)
//   -DPINMAP_PHASE=2  -> only Phase B (data=5  clock=4, vendor theory)
//   (unset)           -> both phases alternating (original combined test)
void loop() {
    static uint32_t cycle = 0;
    Serial.printf("\n[PINMAP] --- cycle %lu ---\n", (unsigned long)cycle);
#if PINMAP_PHASE == 1
    runPhase("A(2/6)", PINS_A_DATA, PINS_A_CLOCK);
#elif PINMAP_PHASE == 2
    runPhase("B(5/4)", PINS_B_DATA, PINS_B_CLOCK);
#else
    runPhase("A(2/6)", PINS_A_DATA, PINS_A_CLOCK);
    runPhase("B(5/4)", PINS_B_DATA, PINS_B_CLOCK);
#endif
    cycle++;
}
