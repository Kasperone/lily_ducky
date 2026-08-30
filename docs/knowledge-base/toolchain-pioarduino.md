# Toolchain — pioarduino, vendored libraries, Arduino-ESP32 3.x drift

## Why pioarduino instead of official PlatformIO

`VERIFIED (source)`. The official `platformio/espressif32` platform's Arduino
core is stuck on the 2.x line and has never added ESP32-C5/C6/H2/P4 targets — see
the community fork announcement
([CNX Software, Aug 2024](https://www.cnx-software.com/2024/08/27/platform-espressif32-fork-platformio-arduino-esp32-c6-esp32-c5-esp32-h2-esp32-p4/))
and, as of this writing (2026-08-30), PlatformIO's own OSS update posts still
don't announce native C5 support landing upstream — the pioarduino fork
(`github.com/pioarduino/platform-espressif32`) remains the only path. This repo
pins a specific release in `firmware/platformio.ini`:

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
```

Pinning by release URL (not `~55.03.311` or a branch) means upgrades are a
deliberate, reviewed action — check the pioarduino release notes before bumping,
since C5 support there is still comparatively new and can regress.

## Vendored, not `lib_deps`

Two things are vendored directly into `firmware/` instead of being pulled by
PlatformIO's library manager, both for the same reason — a required patch that
the upstream registry copy doesn't carry:

- `firmware/boards/*.json` — LilyGO board definitions aren't in pioarduino's
  board index at all.
- `firmware/lib/TFT_eSPI/` — patched for C5 support; see
  `peripherals-lcd-tft-espi.md`. The registry copy would silently shadow the
  patch on any `pio lib install` / clean checkout.

If you add a new library and are tempted to vendor it "to be safe," don't —
vendor only when there's a specific patch or missing-upstream-support reason,
per `docs/programming-style.md`'s "earn your abstractions" rule extended to
dependencies. Document the reason at the vendoring site the way both of the
above do.

## Arduino-ESP32 3.x API drift — concrete catalog

This project was originally written against 2.x-era API assumptions and ported to
3.x (IDF 5.5, via pioarduino) in commits `c489531` and `fb6ec7b`. These are
first-hand, compile-verified (not just researched) drift points — useful as a
checklist if you hit similar errors elsewhere in the codebase, or if a library
example you find online still shows 2.x-era calls:

| API (2.x) | 3.x replacement | Where fixed |
|---|---|---|
| `class USB usb; usb.begin();` | single global `extern ESPUSB USB;` — call `USB.begin()` directly, no local instance | `hal.cpp`, commit `c489531` |
| `USBHIDKeyboard::press(keycode, modifier)` (2-arg) | `press(ascii)` is now ASCII-only 1-arg; for raw modifier+scancode, build a `KeyReport{modifier, 0, {keycode,...}}` and call `sendReport(&report)` | `hal.cpp`, commit `c489531` |
| `keyboard.getLEDsStatus()` polling | removed; subscribe to `ARDUINO_USB_HID_KEYBOARD_LED_EVENT` via `keyboard.onEvent(...)`, read `arduino_usb_hid_keyboard_event_data_t::leds` in the callback | `hal.cpp`, commit `c489531` |
| `File::name()` returning `String` | returns `const char*`; use `strlen()`/`strcpy` instead of `String` methods | `storage.cpp`, commit `fb6ec7b` |
| UTF-8 multibyte char literals (`'€'`, `'°'`, `'ß'`) in a `char` table | narrowing-conversion compile error under 3.x's stricter GCC config, and were dead code anyway (the interpreter feeds one byte at a time so multibyte glyphs could never match) — deleted, not worked around | `interpreter.cpp`, commit `fb6ec7b` |
| declaring `class Foo;` *inside* a `namespace Bar { ... }` when you mean the global `::Foo` | GCC treats it as declaring a **new** incomplete type `Bar::Foo` — forward-declare at global scope, above the namespace | `web_server.h`, commit `fb6ec7b` |

**Known unresolved drift** (documented, not fixed — needs a custom board variant
or core patch, out of scope for a firmware-logic fix): 3.x's
`variants/esp32s3/pins_arduino.h` unconditionally `#define`s `USB_VID`/`USB_PID`,
which silently overrides the `-DUSB_VID=0x046D -DUSB_PID=0xC52B` Logitech-spoof
build flags in `platformio.ini` for the S3 target. Per commit `c489531`'s message,
this is a known, tracked issue — don't "fix" it by hand-patching the vendored
variant header without flagging that as a separate, deliberate change (it would
affect every S3 build, not just this project).
