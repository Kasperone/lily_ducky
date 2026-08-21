// =============================================================================
// main.cpp — DuckPrime: USB Rubber Ducky on the T-Dongle-S3 (ESP32-S3)
// =============================================================================
// Entry point. Wires together: HAL, Storage, Interpreter, Display, C2 WiFi.
// Boot sequence: init → wait for USB enumeration → auto-fire payload from SD.
// =============================================================================

#include <Arduino.h>
#include <USB.h>

// Project modules
#include "config.h"
#include "hal/hal.h"
#include "interpreter/interpreter.h"
#include "storage/storage.h"
#include "c2/web_server.h"
#include "display/display.h"

// ── Globals ─────────────────────────────────────────────────────────────────
static Interpreter interp;

// ── Auto-detect mode ────────────────────────────────────────────────────────
// Fire payload on plug-in (CFG_AUTO_FIRE_ON_PLUG=1)
// OR listen for C2 commands over WiFi
// =============================================================================

static void fireDefaultPayload()
{
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
}

static void startWiFi()
{
    if (WebServer::start()) {
        WebServer::setInterpreter(&interp);
    }
}

// ── setup ──────────────────────────────────────────────────────────────────
void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[MAIN] DuckPrime v0.1 — T-Dongle-S3");
    Serial.println("       USB Rubber Ducky on ESP32-S3");

    // 1. Init HAL (LED, button, USB descriptors)
    Serial.print("[MAIN] Init HAL... ");
    Hal::init();
    Serial.println("OK");

    // 2. Register USB keyboard
    Serial.print("[MAIN] USB HID keyboard... ");
    Hal::keyboardBegin();
    Serial.println("OK");

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

    // 6. Fire default payload if configured
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
    WebServer::tick();

    // Update display based on current state
    Display::update(
        interp.getState(),
        WebServer::running(),
        WebServer::connectedClients()
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

    // If interpreter finished and we're in a finished state, reset
    if (interp.getState() == INTERP_COMPLETE || interp.getState() == INTERP_ERROR) {
        // Display already handles this
    }
}
