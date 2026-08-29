// =============================================================================
// hal.cpp — Hardware Abstraction Layer implementation
// =============================================================================
// USB HID keyboard (T-Dongle-S3), APA102 RGB LED, button input,
// OS detection, VID/PID spoofing, keystroke capture, USB Mass Storage
// composite. Everything behind CFG_HAS_USB_HID degrades to honest no-ops
// on the T-Dongle-C5 (ESP32-C5 has no USB-OTG peripheral).
// =============================================================================

#include "hal.h"
#include "storage/storage.h"

#if CFG_HAS_USB_HID
#include <USB.h>
#include <USBHIDKeyboard.h>
#endif

// ─── USB Keyboard instance (S3 only) ────────────────────────────────────────
#if CFG_HAS_USB_HID
static USBHIDKeyboard keyboard;
// No local USB device instance: Arduino-ESP32 3.x exposes a single global
// (`extern ESPUSB USB` in USB.h) instead of the 2.x `class USB`.
static void onKeyboardLedEvent(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);
#endif

// ─── VID/PID Spoofing State ──────────────────────────────────────────────────
static uint16_t _activeVID = CFG_USB_VID;
static uint16_t _activePID = CFG_USB_PID;

// ─── OS Detection State (S3 only — C5 has no HID to observe) ────────────────
#if CFG_HAS_USB_HID
static uint32_t _osDetectStartMs = 0;
static bool     _osDetectRunning = false;
static uint8_t  _lastLedState    = 0;
static uint32_t _setReportCount  = 0;
static uint32_t _firstReportTime = 0;
static uint32_t _lastReportTime  = 0;
#endif

// ─── Keystroke Capture State (S3 only) ──────────────────────────────────────
#if CFG_HAS_USB_HID
static bool     _capturing = false;
static uint8_t  _captureBuffer[CFG_EXFIL_BUF_SIZE];
static uint16_t _capturePos = 0;
#endif

// ─── USB MSC State ───────────────────────────────────────────────────────────
// USBMSC class is available in Arduino-ESP32 core (USBMassStorage equivalent)
#if CFG_HAS_USB_HID && __has_include(<USBMSC.h>)
#include <USBMSC.h>
static USBMSC msc;
static bool _mscEnabled = false;

static int32_t mscReadCb(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize);
static int32_t mscWriteCb(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize);
#else
static bool _mscEnabled = false;
#endif

// ─── init ────────────────────────────────────────────────────────────────────
void Hal::init() {
    // Button setup
    pinMode(PIN_BUTTON, INPUT_PULLUP);

    // LED: data and clock pins as outputs (APA102 is SPI-like, bit-banged)
    pinMode(PIN_LED_DATA,  OUTPUT);
    pinMode(PIN_LED_CLOCK, OUTPUT);
    ledOff();

#if CFG_HAS_USB_HID
    // ── VID/PID Spoofing ──────────────────────────────────────────────────────
    // VID/PID are set at COMPILE TIME via platformio.ini build flags:
    //   -DUSB_VID=0x046D  -DUSB_PID=0xC52B
    // The TinyUSB descriptor is generated from these at build time.
    // Runtime identity changes require USB re-enumeration (detach/re-attach).
    Serial.printf("[HAL] USB identity: VID=%04X PID=%04X MFR=%s PROD=%s\n",
                  _activeVID, _activePID, CFG_USB_MFR, CFG_USB_PROD);
    Serial.println("[HAL] (VID/PID set via build flags, see platformio.ini)");
#else
    Serial.println("[HAL] " CFG_BOARD_NAME ": no USB-OTG — HID features disabled");
    Serial.println("[HAL] (ESP32-C5 USB port is a fixed-function Serial/JTAG console)");
#endif
}

// ─── USB Keyboard ────────────────────────────────────────────────────────────
#if CFG_HAS_USB_HID

void Hal::keyboardBegin() {
    keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT, onKeyboardLedEvent);
    USB.begin();                // start USB device, enumerate
    delay(500);                 // let host settle
    keyboard.begin();
}

void Hal::keyboardEnd() {
    keyboard.end();
    // Don't call usb.end() — the USB stack may be needed again
}

void Hal::press(uint8_t modifier, uint8_t keycode) {
    // 3.x press(k) takes ASCII only; modifier+raw-scancode goes through the
    // low-level KeyReport (same wire format the 2.x two-arg press produced).
    KeyReport report = {modifier, 0, {keycode, 0, 0, 0, 0, 0}};
    keyboard.sendReport(&report);

    // ── Keystroke capture: record outgoing keycodes, auto-flushing to SD
    //    when the buffer fills so long sessions don't silently drop data.
    if (_capturing) {
        if (_capturePos + 2 > CFG_EXFIL_BUF_SIZE) {
            if (!Storage::dirExists(SD_LOOT_DIR)) Storage::createDir(SD_LOOT_DIR);
            Storage::appendLoot(_captureBuffer, _capturePos);
            _capturePos = 0;
        }
        _captureBuffer[_capturePos++] = modifier;
        _captureBuffer[_capturePos++] = keycode;
    }
}

void Hal::release() {
    keyboard.releaseAll();
}

void Hal::typeChar(char c) {
    keyboard.write(c);          // handles shift automatically for uppercase/symbols
}

void Hal::typeString(const char* s) {
    keyboard.print(s);          // type each character with natural delay
}

#else  // !CFG_HAS_USB_HID — ESP32-C5: nothing to type with

void Hal::keyboardBegin() {
    Serial.println("[HAL] keyboardBegin: no USB-OTG on this target");
}

void Hal::keyboardEnd() {}

void Hal::press(uint8_t modifier, uint8_t keycode) {
    (void)modifier; (void)keycode;
}

void Hal::release() {}

void Hal::typeChar(char c) { (void)c; }

void Hal::typeString(const char* s) { (void)s; }

#endif

void Hal::delayMs(uint16_t ms) {
    delay(ms);
}

// ─── VID/PID Spoofing ────────────────────────────────────────────────────────
void Hal::spoofIdentity(uint16_t vid, uint16_t pid) {
    _activeVID = vid;
    _activePID = pid;
#if CFG_HAS_USB_HID
    // VID/PID descriptor is baked in at compile time (see platformio.ini).
    // To apply runtime changes: update platformio.ini → rebuild → reflash.
    // This stores the desired values but cannot hot-swap the USB descriptor.
    Serial.printf("[HAL] Spoof identity stored: %04X:%04X (requires rebuild to apply)\n",
                  vid, pid);
#else
    Serial.printf("[HAL] Spoof identity ignored: %04X:%04X (no USB-OTG on this target)\n",
                  vid, pid);
#endif
}

uint16_t Hal::getVID() { return _activeVID; }
uint16_t Hal::getPID() { return _activePID; }

// ─── OS Detection ────────────────────────────────────────────────────────────
// Strategy: count keyboard LED output reports during the enumeration window.
// - Windows typically sends 3 SET_REPORT (Caps, Num, Scroll lock state)
// - macOS sends fewer/faster
// - Linux sends 1-2, slower timing
// 3.x note: getLEDsStatus() was removed; LED output reports now arrive as
// ARDUINO_USB_HID_KEYBOARD_LED_EVENT events, which are more precise than the
// old poll anyway (each event is one host SET_REPORT).
#if CFG_HAS_USB_HID

static void onKeyboardLedEvent(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    (void)arg; (void)event_base; (void)event_id;
    if (!_osDetectRunning) return;
    arduino_usb_hid_keyboard_event_data_t* data =
        (arduino_usb_hid_keyboard_event_data_t*)event_data;
    if (data->leds != _lastLedState) {
        if (_setReportCount == 0) _firstReportTime = millis();
        _lastReportTime = millis();
        _setReportCount++;
        _lastLedState = data->leds;
    }
}

void Hal::osDetectStart() {
    _setReportCount  = 0;
    _firstReportTime = 0;
    _lastReportTime  = 0;
    _lastLedState    = 0;
    _osDetectStartMs = millis();
    _osDetectRunning = true;
    Serial.println("[HAL] OS detection started");
}

void Hal::osDetectTick() {
    if (!_osDetectRunning) return;

    // Check if detection window has elapsed
    if (millis() - _osDetectStartMs > CFG_OS_DETECT_WINDOW_MS) {
        _osDetectRunning = false;
        Serial.printf("[HAL] OS detection done: %lu report(s) in %lu ms → %s\n",
                      (unsigned long)_setReportCount,
                      (unsigned long)(_lastReportTime - _firstReportTime),
                      osDetectResult() == OS_WINDOWS ? "WINDOWS" :
                      osDetectResult() == OS_MACOS   ? "MACOS"   :
                      osDetectResult() == OS_LINUX   ? "LINUX"   : "UNKNOWN");
        return;
    }
    // LED events are counted in onKeyboardLedEvent; nothing to poll here.
}

uint8_t Hal::osDetectResult() {
    uint32_t span = _lastReportTime - _firstReportTime;

    // Windows: 3+ SET_REPORT calls, typically spread over 500-2000ms
    if (_setReportCount >= 3) return OS_WINDOWS;

    // macOS: fast enumeration, often 2+ calls within 500ms
    if (_setReportCount >= 2 && span < 500) return OS_MACOS;

    // Linux: fewer calls (1-2), slower
    if (_setReportCount >= 1) return OS_LINUX;

    // ChromeOS: typically behaves like Linux but with even fewer reports
    // Hard to distinguish without deeper heuristics
    return OS_UNKNOWN;
}

uint8_t Hal::osDetectCount() {
    return (uint8_t)_setReportCount;
}

#else  // !CFG_HAS_USB_HID

void Hal::osDetectStart() {
    Serial.println("[HAL] OS detect: unavailable (no USB HID on this target)");
}
void Hal::osDetectTick() {}
uint8_t Hal::osDetectResult() { return OS_UNKNOWN; }
uint8_t Hal::osDetectCount() { return 0; }

#endif

// ─── Keystroke Capture ───────────────────────────────────────────────────────
#if CFG_HAS_USB_HID

void Hal::captureBegin() {
    _capturePos = 0;
    _capturing = true;
    Serial.println("[HAL] Keystroke capture started");
}

void Hal::captureEnd() {
    _capturing = false;
    if (_capturePos > 0) {
        // Ensure loot directory exists
        if (!Storage::dirExists(SD_LOOT_DIR)) {
            Storage::createDir(SD_LOOT_DIR);
        }
        bool ok = Storage::appendLoot(_captureBuffer, _capturePos);
        Serial.printf("[HAL] Keystroke capture: %u bytes → %s\n",
                      _capturePos, ok ? "SD" : "FAIL");
        _capturePos = 0;
    }
}

bool Hal::isCapturing() {
    return _capturing;
}

#else

void Hal::captureBegin() {
    Serial.println("[HAL] EXFIL: capture unavailable (no USB HID on this target)");
}
void Hal::captureEnd() {}
bool Hal::isCapturing() { return false; }

#endif

// ─── USB MSC (Composite HID+Storage) ─────────────────────────────────────────
// Gated behind ENABLE_MSC because the read/write callbacks below are stubs:
// reads return zeros and writes are discarded. Enumerating MSC in that state
// would let the host *write* to a phantom drive and silently lose data, which
// is worse than not exposing storage at all. Wire SD raw-sector access
// (readRAW/writeRAW) before defining ENABLE_MSC.
#if defined(ENABLE_MSC) && CFG_HAS_USB_HID && __has_include(<USBMSC.h>)

static int32_t mscReadCb(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    // TODO: SD_MMC.readRAW(buffer, lba) once available on this core.
    (void)lba; (void)offset;
    memset(buffer, 0, bufsize);
    return bufsize;
}

static int32_t mscWriteCb(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    // TODO: SD_MMC.writeRAW(buffer, lba) once available on this core.
    (void)lba; (void)offset; (void)buffer;
    return bufsize;
}

bool Hal::enableMSC() {
    if (_mscEnabled) return true;
    if (!Storage::ready()) {
        Serial.println("[HAL] MSC: SD card not available");
        return false;
    }

    uint64_t cardSize = Storage::cardSize();
    uint32_t blockCount = (uint32_t)(cardSize / 512);
    if (blockCount == 0) blockCount = 2048;

    msc.begin(blockCount, 512);
    msc.onRead(mscReadCb);
    msc.onWrite(mscWriteCb);

    _mscEnabled = true;
    Serial.printf("[HAL] MSC enabled: %u sectors (512B each)\n", blockCount);
    return true;
}

void Hal::disableMSC() {
    if (!_mscEnabled) return;
    msc.end();
    _mscEnabled = false;
    Serial.println("[HAL] MSC disabled");
}

bool Hal::mscActive() {
    return _mscEnabled;
}

#else  // MSC opt-in not set, no USB-OTG, or USBMSC not available

bool Hal::enableMSC() {
#if !CFG_HAS_USB_HID
    Serial.println("[HAL] MSC: no USB-OTG on this target");
#elif !defined(ENABLE_MSC)
    Serial.println("[HAL] MSC: disabled by build (define ENABLE_MSC after wiring SD raw I/O)");
#else
    Serial.println("[HAL] MSC: USBMSC.h not available in this Arduino-ESP32 core");
#endif
    return false;
}
void Hal::disableMSC() {}
bool Hal::mscActive() { return false; }

#endif

// ─── LED (APA102, BGR colour order) ─────────────────────────────────────────
// Global brightness for the APA102 5-bit current register. LilyGO's factory
// firmware uses 10 (of 31) to avoid blinding output on the 2020-package LED;
// 0 blanks the LED entirely regardless of RGB (the frame header is
// 111bbbbb — an earlier version shipped bbbbb=00000, killing all feedback).
#define HAL_APA102_BRIGHTNESS 10

static void sendAPA102(uint8_t r, uint8_t g, uint8_t b) {
    // APA102: start frame, LED frame, end frame
    // LED frame: 111bbbbb gggggggg rrrrrrrr bbbbbbbb
    //   bbbbb = global brightness (31 = full)
    // Start frame: 32 zero bits
    pinMode(PIN_LED_CLOCK, OUTPUT);
    pinMode(PIN_LED_DATA, OUTPUT);
    digitalWrite(PIN_LED_CLOCK, LOW);
    digitalWrite(PIN_LED_DATA, LOW);
    for (int i = 0; i < 32; i++) {
        digitalWrite(PIN_LED_CLOCK, HIGH);
        digitalWrite(PIN_LED_CLOCK, LOW);
    }

    // LED frame: header(0xE0 | brightness) + B + G + R
    uint32_t frame = (0xE0UL | HAL_APA102_BRIGHTNESS) << 24
                   | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    for (int i = 31; i >= 0; i--) {
        digitalWrite(PIN_LED_DATA, (frame >> i) & 1);
        digitalWrite(PIN_LED_CLOCK, HIGH);
        digitalWrite(PIN_LED_CLOCK, LOW);
    }

    // End frame: 32 one bits or at least (N/2) one bits
    digitalWrite(PIN_LED_DATA, HIGH);
    for (int i = 0; i < 32; i++) {
        digitalWrite(PIN_LED_CLOCK, HIGH);
        digitalWrite(PIN_LED_CLOCK, LOW);
    }
    digitalWrite(PIN_LED_DATA, LOW);
}

// Cache last-sent colour so the bit-banged APA102 send doesn't re-toggle
// ~100+ GPIO lines on every loop() iteration when nothing has changed.
static uint8_t _lastR = 1, _lastG = 1, _lastB = 1;  // sentinel != (0,0,0)

void Hal::ledSet(uint8_t r, uint8_t g, uint8_t b) {
    if (r == _lastR && g == _lastG && b == _lastB) return;
    _lastR = r; _lastG = g; _lastB = b;
    sendAPA102(r, g, b);
}

void Hal::ledOff() {
    ledSet(0, 0, 0);
}

void Hal::ledBlink(uint8_t r, uint8_t g, uint8_t b, uint8_t count, uint16_t periodMs) {
    for (uint8_t i = 0; i < count; i++) {
        ledSet(r, g, b);
        delay(periodMs);
        ledOff();
        delay(periodMs);
    }
}

void Hal::statusIdle()    { ledSet(80, 80, 0);   }   // amber
void Hal::statusRunning() { ledSet(0, 255, 0);    }   // green (pulsing)
void Hal::statusComplete(){ ledSet(0, 255, 0);     }   // solid green
void Hal::statusError()   { ledSet(255, 0, 0);     }   // red
void Hal::statusWiFi()    { ledSet(0, 120, 255);   }   // cyan/teal

// ─── Button ──────────────────────────────────────────────────────────────────
bool Hal::buttonPressed() {
    // Simple debounce: read state and return once
    static bool prevState = true;  // pulled UP = idle HIGH
    bool currentState = digitalRead(PIN_BUTTON);

    bool pressed = false;
    if (prevState == HIGH && currentState == LOW) {
        // Was high, now low = fresh press
        delay(50);  // debounce
        if (digitalRead(PIN_BUTTON) == LOW) {
            pressed = true;
        }
    }
    prevState = currentState;
    return pressed;
}
