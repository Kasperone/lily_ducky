# USB-CDC boot race and the C5 console

## The problem

On the C5, the USB port is the fixed-function `USB_SERIAL_JTAG` controller
running as CDC-ACM (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1` in
`platformio.ini`). Bytes written to `Serial` before the host's OS finishes
enumerating the CDC device are simply dropped — there's no buffering across
enumeration. This project prints the C2 WiFi auth token once, at boot, over that
same `Serial` — if it's dropped, the token is unrecoverable without a reset
(`README.md`, `AGENTS.md`).

This is a known, common Arduino-ESP32 pain point, not specific to this repo or
the C5 — see
[espressif/arduino-esp32#8238](https://github.com/espressif/arduino-esp32/issues/8238)
("ESP32-S3 does not boot until Serial console is attached (Using CDC on Boot)")
and related discussion. `VERIFIED (source)` that this class of issue is
well-documented upstream, `PLAUSIBLE (unverified)` that this repo's specific fix
behaves correctly on real hardware — the boot-log/token-print flow hasn't been
independently re-confirmed against a live serial capture in this session.

## The fix in this repo

`firmware/src/main.cpp`, gated on `ARDUINO_USB_MODE` (so it only runs for the C5
build; the S3 uses TinyUSB native USB and intentionally stays headless-fast, per
the comment there, for its payload auto-fire timing):

```c
uint32_t waitStart = millis();
while (!Serial && millis() - waitStart < CFG_SERIAL_CONNECT_WAIT_MS) {
    delay(10);
}
```

`CFG_SERIAL_CONNECT_WAIT_MS` is 8000 (`config.h`). This matches the general shape
of the community workaround pattern reported around
`arduino-esp32#8238`/`#1102` (`while (!Serial && millis() < deadline)`), and the
project's own reasoning is sound: a timeout is required so a **headless** boot
(dongle plugged into a charger, no monitoring host) still completes instead of
hanging forever waiting for a `Serial` connection that will never come.

**Things worth knowing if this needs revisiting:**

- `while (!Serial)` semantics changed across Arduino-ESP32 core versions — some
  older cores treat `Serial` as truthy as soon as the CDC *endpoint* exists
  (before a terminal actually opens it), which would make this wait a no-op in
  practice. Confirm against the exact core version pinned by the pioarduino
  release in `toolchain-pioarduino.md` if the token is still reported lost.
- 8 seconds is long enough to be noticeable on a bench (plug in, watch the LED/LCD
  sit idle) but short enough not to matter for a "leave it plugged in and forget
  it" WiFi-C2 use case. If this becomes a UX complaint, the fix is almost never
  "add more delay logic" — it's exposing the token some other way (LCD screen,
  which this project already has, per `AGENTS.md`'s LCD roadmap item "live
  OS/layout, current DuckyScript line" — auth token display isn't on that list
  yet but is a natural fit given the LCD exists specifically to avoid depending
  on the serial console).
