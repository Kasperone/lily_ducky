// =============================================================================
// main.cpp — LilyDucky: USB Rubber Ducky on the LILYGO T-Dongle family
// =============================================================================
// Entry point. Wires together: HAL, Storage, Interpreter, Display, C2 WiFi.
//
// Boot sequence:
//   T-Dongle-S3 (CFG_HAS_USB_HID=1): init → USB enumerate → auto-fire payload
//   T-Dongle-C5 (CFG_HAS_USB_HID=0): init → WiFi C2 lab node. The C5 has no
//   USB-OTG peripheral, so payloads still parse and execute but type nothing;
//   the device is exercised through the dashboard, LCD, and SD card.
// =============================================================================

#include <Arduino.h>

// Project modules
#include "config.h"
#include "hal/hal.h"
#include "interpreter/interpreter.h"
#include "storage/storage.h"
#include "c2/web_server.h"
#include "display/display.h"
#if CFG_C2_SELFTEST
#include "c2/c2_selftest.h"
#endif

// ── Globals ─────────────────────────────────────────────────────────────────
static Interpreter interp;

// ── Auto-detect mode ────────────────────────────────────────────────────────
// Fire payload on plug-in (CFG_AUTO_FIRE_ON_PLUG=1)
// OR listen for C2 commands over WiFi
// =============================================================================

static void fireDefaultPayload()
{
#if CFG_HAS_USB_HID
    char* buf = NULL;
    int len = 0;

    // Try CFG_PAYLOAD_FILENAME from SD first
    if (Storage::payloadExists(CFG_PAYLOAD_FILENAME)) {
        if (Storage::loadPayload(CFG_PAYLOAD_FILENAME, &buf, &len)) {
            Serial.printf("[MAIN] Loading %s (%d bytes)\n", CFG_PAYLOAD_FILENAME, len);
            // Give host time after enumeration
            delay(CFG_ENUMERATION_DELAY_MS);

            // ── OS Detection: run during enumeration delay ──────────────
            Hal::osDetectStart();
            uint32_t detectStart = millis();
            while (millis() - detectStart < CFG_OS_DETECT_WINDOW_MS) {
                Hal::osDetectTick();
                delay(50);
            }
            // Inject built-in variables before payload runs
            interp.setBuiltinVar("_OS", Hal::osDetectResult());
            interp.setBuiltinVar("_HOST_CONFIGURATION_REQUEST_COUNT", Hal::osDetectCount());

            Hal::statusRunning();
            interp.loadBuffer(buf, len);
            // Re-inject after loadBuffer (it resets vars)
            interp.setBuiltinVar("_OS", Hal::osDetectResult());
            interp.setBuiltinVar("_HOST_CONFIGURATION_REQUEST_COUNT", Hal::osDetectCount());
            free(buf);
            interp.run();
            return;
        }
    }

    // Fallback: try any .dd in payloads dir
    String list;
    if (Storage::listPayloads(&list, 512) && list.length() > 0) {
        // Parse first payload name (comma-separated)
        int comma = list.indexOf(',');
        String name = comma > 0 ? list.substring(0, comma) : list;
        if (Storage::loadPayload(name.c_str(), &buf, &len)) {
            Serial.printf("[MAIN] Loading %s (%d bytes)\n", name.c_str(), len);
            delay(CFG_ENUMERATION_DELAY_MS);
            Hal::statusRunning();
            interp.loadBuffer(buf, len);
            interp.setBuiltinVar("_OS", Hal::osDetectResult());
            interp.setBuiltinVar("_HOST_CONFIGURATION_REQUEST_COUNT", Hal::osDetectCount());
            free(buf);
            interp.run();
            return;
        }
    }

    // No payload found → error blink
    Serial.println("[MAIN] No payload found on SD");
    Hal::ledBlink(255, 0, 0, 3, 200);
    Hal::statusIdle();
#else
    // C5: no USB HID — the payload still parses and executes, but typing is
    // a no-op. Keeps the interpreter state machine (LED, LCD, timing) fully
    // exercisable for learning and dashboard testing.
    char* buf = NULL;
    int len = 0;
    if (Storage::payloadExists(CFG_PAYLOAD_FILENAME) &&
        Storage::loadPayload(CFG_PAYLOAD_FILENAME, &buf, &len)) {
        Serial.printf("[MAIN] Loading %s (%d bytes) — typing is a no-op on this target\n",
                      CFG_PAYLOAD_FILENAME, len);
        Hal::statusRunning();
        interp.loadBuffer(buf, len);
        free(buf);
        interp.run();
        return;
    }
    Serial.println("[MAIN] No payload found on SD");
    Hal::ledBlink(255, 0, 0, 3, 200);
    Hal::statusIdle();
#endif
}

static void startWiFi()
{
    if (C2Server::start()) {
        C2Server::setInterpreter(&interp);
    }
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);

#if ARDUINO_USB_MODE
    // USB Serial/JTAG console (C5): bytes printed before the host enumerates
    // are dropped — that loses the boot log and, critically, the C2 auth
    // token, which cannot be recovered without another reset. Wait for the
    // CDC link with a timeout so a headless boot still completes.
    // (S3 uses TinyUSB native USB and must stay headless-fast for its
    // payload auto-fire timing, so it skips the wait.)
    {
        uint32_t waitStart = millis();
        while (!Serial && millis() - waitStart < CFG_SERIAL_CONNECT_WAIT_MS) {
            delay(10);
        }
    }
#endif

    Serial.println("\n[MAIN] LilyDucky v0.2 — " CFG_BOARD_NAME);
    Serial.println("       USB Rubber Ducky lab on " CFG_MCU_NAME);

    // 1. Init HAL (LED, button, USB descriptors where available)
    Serial.print("[MAIN] Init HAL... ");
    Hal::init();
    Serial.println("OK");

    // 2. Register USB keyboard (no-op on C5)
    Serial.print("[MAIN] USB HID keyboard... ");
    Hal::keyboardBegin();
    Serial.println(CFG_HAS_USB_HID ? "OK" : "N/A");

    // 3. Init SD card (optional — system works without it, just no payload files)
    Serial.print("[MAIN] SD card... ");
    bool sdOk = Storage::init();
    if (sdOk) {
        // Ensure payloads directory exists
        if (!Storage::dirExists(SD_PAYLOAD_DIR)) {
            Storage::createDir(SD_PAYLOAD_DIR);
        }
        // Ensure loot directory exists (for exfiltration)
        if (!Storage::dirExists(SD_LOOT_DIR)) {
            Storage::createDir(SD_LOOT_DIR);
        }
    } else {
        Serial.println("FAILED — payload files unavailable, C2 still works");
    }

    // 4. Init display
    Display::init();

    // 5. Start WiFi C2 (SoftAP)
    Serial.print("[MAIN] WiFi C2... ");
    startWiFi();
    Hal::statusWiFi();

#if CFG_C2_SELFTEST
    // Loopback end-to-end test of the C2 REST API (own SoftAP IP, no external
    // WiFi client). Spawns its own task; runs once, ~1.5 s after boot, while
    // loop() below services the server. Off in the default build.
    C2SelfTest::begin();
#endif

    // 6. Fire default payload if configured (S3 only; C5 has no HID)
#if CFG_AUTO_FIRE_ON_PLUG
    if (sdOk) {
        fireDefaultPayload();
    }
#endif

    Serial.println("[MAIN] Boot complete");
}

// ── loop ───────────────────────────────────────────────────────────────────
void loop()
{
    // Tick interpreter (non-blocking: runs one step per tick if still going)
    interp.tick();

    // Tick OS detection (non-blocking, if running)
    Hal::osDetectTick();

    // Tick WiFi C2
    C2Server::tick();

    // Update display based on current state
    Display::update(
        interp.getState(),
        C2Server::running(),
        C2Server::connectedClients()
    );

    // Check button press → trigger payload (alternative to auto-fire)
    if (Hal::buttonPressed()) {
        if (interp.getState() == INTERP_RUNNING) {
            interp.stop();
            Serial.println("[MAIN] Button: stopped payload");
        } else {
            fireDefaultPayload();
            Serial.println("[MAIN] Button: started payload");
        }
        delay(200);  // debounce
    }
}
