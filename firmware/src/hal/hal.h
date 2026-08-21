// =============================================================================
// hal.h / hal.cpp — Hardware Abstraction Layer
// =============================================================================
// USB HID keyboard (S3 only), APA102 RGB LED, button input.
// All platform-specific so the interpreter doesn't touch Arduino APIs.
//
// On the T-Dongle-C5 (no USB-OTG peripheral) every keyboard function is a
// documented no-op: press() types nothing, osDetectResult() reports
// OS_UNKNOWN, enableMSC() refuses. The interpreter and C2 code paths stay
// identical across both boards.
// =============================================================================
#ifndef LILLY_DUCKY_HAL_H
#define LILLY_DUCKY_HAL_H

#include "config.h"
#include <Arduino.h>

// ─── Keyboard wrapper ────────────────────────────────────────────────────────

// HID keycodes — same values the Arduino-ESP32 USBHID Keyboard class uses
// so we bypass the library and build descriptors ourselves for full control.

// Modifier masks
static const uint8_t KEY_MOD_NONE   = 0x00;
static const uint8_t KEY_MOD_LCTRL  = 0x01;
static const uint8_t KEY_MOD_LSHIFT = 0x02;
static const uint8_t KEY_MOD_LALT   = 0x04;
static const uint8_t KEY_MOD_LGUI   = 0x08;
static const uint8_t KEY_MOD_RCTRL  = 0x10;
static const uint8_t KEY_MOD_RSHIFT = 0x20;
static const uint8_t KEY_MOD_RALT   = 0x40;
static const uint8_t KEY_MOD_RGUI   = 0x80;

// ASCII-to-HID conversion table and special keys
static const uint8_t KEY_NONE            = 0x00;
static const uint8_t KEY_A               = 0x04;
static const uint8_t KEY_B               = 0x05;
static const uint8_t KEY_C               = 0x06;
static const uint8_t KEY_D               = 0x07;
static const uint8_t KEY_E               = 0x08;
static const uint8_t KEY_F               = 0x09;
static const uint8_t KEY_G               = 0x0A;
static const uint8_t KEY_H               = 0x0B;
static const uint8_t KEY_I               = 0x0C;
static const uint8_t KEY_J               = 0x0D;
static const uint8_t KEY_K               = 0x0E;
static const uint8_t KEY_L               = 0x0F;
static const uint8_t KEY_M               = 0x10;
static const uint8_t KEY_N               = 0x11;
static const uint8_t KEY_O               = 0x12;
static const uint8_t KEY_P               = 0x13;
static const uint8_t KEY_Q               = 0x14;
static const uint8_t KEY_R               = 0x15;
static const uint8_t KEY_S               = 0x16;
static const uint8_t KEY_T               = 0x17;
static const uint8_t KEY_U               = 0x18;
static const uint8_t KEY_V               = 0x19;
static const uint8_t KEY_W               = 0x1A;
static const uint8_t KEY_X               = 0x1B;
static const uint8_t KEY_Y               = 0x1C;
static const uint8_t KEY_Z               = 0x1D;

static const uint8_t KEY_1 = 0x1E;
static const uint8_t KEY_2 = 0x1F;
static const uint8_t KEY_3 = 0x20;
static const uint8_t KEY_4 = 0x21;
static const uint8_t KEY_5 = 0x22;
static const uint8_t KEY_6 = 0x23;
static const uint8_t KEY_7 = 0x24;
static const uint8_t KEY_8 = 0x25;
static const uint8_t KEY_9 = 0x26;
static const uint8_t KEY_0 = 0x27;

// Named keys
static const uint8_t KEY_RETURN    = 0x28;
static const uint8_t KEY_ESC       = 0x29;
static const uint8_t KEY_BACKSPACE = 0x2A;
static const uint8_t KEY_TAB       = 0x2B;
static const uint8_t KEY_SPACE     = 0x2C;
static const uint8_t KEY_MINUS     = 0x2D;
static const uint8_t KEY_EQUAL     = 0x2E;
static const uint8_t KEY_LBRACK    = 0x2F;
static const uint8_t KEY_RBRACK    = 0x30;
static const uint8_t KEY_BSLASH    = 0x31;
static const uint8_t KEY_SEMI      = 0x33;
static const uint8_t KEY_QUOTE     = 0x34;
static const uint8_t KEY_GRAVE     = 0x35;
static const uint8_t KEY_COMMA     = 0x36;
static const uint8_t KEY_DOT       = 0x37;
static const uint8_t KEY_SLASH     = 0x38;

static const uint8_t KEY_CAPSLOCK    = 0x39;
static const uint8_t KEY_F1          = 0x3A;
static const uint8_t KEY_F2          = 0x3B;
static const uint8_t KEY_F3          = 0x3C;
static const uint8_t KEY_F4          = 0x3D;
static const uint8_t KEY_F5          = 0x3E;
static const uint8_t KEY_F6          = 0x3F;
static const uint8_t KEY_F7          = 0x40;
static const uint8_t KEY_F8          = 0x41;
static const uint8_t KEY_F9          = 0x42;
static const uint8_t KEY_F10         = 0x43;
static const uint8_t KEY_F11         = 0x44;
static const uint8_t KEY_F12         = 0x45;
static const uint8_t KEY_PRINTSCREEN = 0x46;
static const uint8_t KEY_SCROLLLOCK  = 0x47;
static const uint8_t KEY_PAUSE       = 0x48;

static const uint8_t KEY_INSERT  = 0x49;
static const uint8_t KEY_HOME    = 0x4A;
static const uint8_t KEY_PGUP    = 0x4B;
static const uint8_t KEY_DELETE  = 0x4C;
static const uint8_t KEY_END     = 0x4D;
static const uint8_t KEY_PGDOWN  = 0x4E;

static const uint8_t KEY_RIGHT  = 0x4F;
static const uint8_t KEY_LEFT   = 0x50;
static const uint8_t KEY_DOWN   = 0x51;
static const uint8_t KEY_UP     = 0x52;

static const uint8_t KEY_NUMLOCK  = 0x53;

// ── HAL interface ────────────────────────────────────────────────────────────

namespace Hal {

    // One-time init: USB descriptors, LED pins, button
    void init();

    // USB — keyboard (no-ops on targets without USB-OTG, e.g. ESP32-C5)
    void keyboardBegin();           // register HID report descriptors
    void keyboardEnd();             // remove keyboard (keep storage etc.)
    void press(uint8_t modifier, uint8_t keycode);
    void release();                 // send all-keys-up
    void typeChar(char c);          // auto-shift for printable ASCII
    void typeString(const char* s);
    void delayMs(uint16_t ms);

    // USB — VID/PID spoofing (applied at init, can re-apply with new values)
    void spoofIdentity(uint16_t vid, uint16_t pid);
    uint16_t getVID();
    uint16_t getPID();

    // USB — composite HID + Mass Storage (ATTACKMODE)
    bool enableMSC();               // expose SD as USB drive, returns false if SD unavailable
    void disableMSC();              // stop exposing, reclaim SD for firmware use
    bool mscActive();

    // OS Detection — polls LED report changes during enumeration
    void osDetectStart();           // call right after keyboardBegin()
    void osDetectTick();            // call in loop() during detection window
    uint8_t osDetectResult();       // returns OS_* constant from config.h
    uint8_t osDetectCount();        // SET_REPORT count observed

    // Keystroke capture — records our own typed keys to loot file
    void captureBegin();            // start capturing outgoing keystrokes
    void captureEnd();              // stop and flush to SD
    bool isCapturing();

    // LED (APA102)
    // colour order is BGR; each channel 0–255
    void ledSet(uint8_t r, uint8_t g, uint8_t b);
    void ledOff();
    void ledBlink(uint8_t r, uint8_t g, uint8_t b, uint8_t count, uint16_t periodMs);

    // Status helpers — semantic colours (green=ok, red=err, cyan=WiFi, amber=idle)
    void statusIdle();
    void statusRunning();
    void statusComplete();
    void statusError();
    void statusWiFi();

    // Button
    bool buttonPressed();           // true once per physical press (debounced)

} // namespace Hal

#endif // LILLY_DUCKY_HAL_H
