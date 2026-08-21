# funny_usb — DIY BadUSB on the LILYGO T-Dongle-S3

A USB Rubber Ducky clone built for the LILYGO T-Dongle-S3 (ESP32-S3).
Plugs in as a keyboard, executes DuckyScript payloads at human speed, and
optionally serves a WiFi C2 web dashboard for remote control.

## Quick start

1. Install the [Arduino-ESP32 core](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html) v3.3+.
2. Install the [PlatformIO](https://platformio.org/) CLI or the VS Code extension.
3. `git clone` this repo, `cd firmware` then `pio run --target upload`.
4. Place `.dd` payload files on the SD card.
5. Plug into a target and wait 2–3 s for HID enumeration.

## Project structure

```
firmware/                ESP32-S3 Arduino firmware
  platformio.ini          Build config & library deps
  partitions.csv          Partition table (16MB Flash)
  src/
    main.cpp              Entry point, state machine
    config.h              Pinouts, compile-time settings
    hal/
      hal.h / hal.cpp     Hardware abstraction (LED, button, USB HID)
    interpreter/
      interpreter.h       DuckyScript parser
      interpreter.cpp     Tier 1 + Tier 2 command execution
    c2/
      web_server.h        WiFi C2 server
      web_server.cpp      HTTP + REST API + dashboard HTML
    display/
      display.h           RGB LED + ST7735 LCD status (160x80 landscape)
    storage/
      storage.h           SD card payload management
payloads/                 Sample .dd files for testing
```

## DuckyScript support

**Tier 1** — `REM`, `STRING`, `STRINGLN`, `DELAY`, `ENTER`, `SPACE`,
`TAB`, `ESCAPE`, `BACKSPACE`, arrows, `GUI`, `CTRL`, `ALT`, `SHIFT`,
`F1–F12`, `PRINTSCREEN`, `CAPSLOCK`, `NUMLOCK`, combos.

**Tier 2** — `DEFINE`, `VAR`, `IF/ELSE_IF/ELSE/END_IF`, `WHILE/END_WHILE`,
`FUNCTION/CALL/END_FUNCTION`, `RESTART_PAYLOAD`, `STOP_PAYLOAD`,
`DEFAULTDELAY`, `DEFAULT_CHAR_DELAY`.

## Safety

Build and test only in your own lab. All payloads are defensive-security
educational material. See the research doc for the full context.

The C2 SoftAP defaults to SSID `DuckC2` / password `quackquack` (defined in
`firmware/src/config.h`). **Change these before flashing for any test that
isn't fully air-gapped** — anyone in radio range can otherwise associate.
Mutating C2 routes (`PUT /api/payload/*`, `POST /api/run/*`, `POST /api/stop`)
require an auth token that the firmware generates at boot and prints to the
USB-CDC serial console; paste it when the dashboard prompts. The dashboard is
served over plain HTTP (no TLS), which is expected for an embedded SoftAP.
