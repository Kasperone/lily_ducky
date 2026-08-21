# funny_usb — DIY BadUSB on T-Dongle-S3

Research doc: `/home/kasperone/Documents/vault/Claude Code Research/DIY-Rubber-Ducky-BadUSB-Deep-Research.md`

## Status
Phase 1-2 code written (interpreter + C2 + HAL). Hardware not yet acquired. Build not tested.
Phase 4 security bypass features implemented (OS detect, layouts, VID/PID, jitter, exfil, ATTACKMODE).

## Architecture
- **Target**: LILYGO T-Dongle-S3 (ESP32-S3, 16MB Flash, ST7735 LCD, APA102 LED, SD card)
- **Lang**: C/C++ Arduino framework (ESP32 core v6.12.0)
- **USB HID**: `USBHIDKeyboard` (built into Arduino-ESP32 core)
- **WiFi C2**: SoftAP + built-in `WebServer` (dashboard + REST API)
- **Storage**: `SD_MMC` 1-bit mode (CLK=12, CMD=16, D0=14)
- **LCD**: ST7735 0.96" 80×160 IPS via SPI — TFT_eSPI, `LCD_ENABLED=1` in config.h. Landscape (160×80), shows DuckPrime title, SSID, IP, auth token, client count, status corner
- **External libs**: `bodmer/TFT_eSPI` only (single dep; chosen over hand-rolled ST7735 init for board-validated correctness). Everything else ships with Arduino-ESP32 core

## Security bypass features (DuckyScript commands)
- **OS Detection**: `DETECT_OS` → sets $_OS (WINDOWS=1, MACOS=2, LINUX=3), $_HOST_CONFIGURATION_REQUEST_COUNT
  - Auto-runs at boot before payload fires; also callable mid-payload
  - Polls keyboard LED status register during 3s enumeration window
  - Cooperative: the interpreter yields to `loop()` (and therefore `WebServer::tick()`) during the wait
- **Keyboard Layouts**: `LAYOUT US|PL|DE` → switches ASCII-to-HID scancode mapping
  - PL (Programmer's) = US base + AltGr diacritics
  - DE (QWERTZ) = Y/Z swap, AltGr special chars, shifted punctuation
- **Jitter**: `JITTER_MAX <ms>` → random 0-N ms delay between keystrokes (human-like timing)
- **VID/PID Spoofing**: compile-time via platformio.ini (`-DUSB_VID=0x046D -DUSB_PID=0xC52B`)
  - Logitech Unifying Receiver preset; Dell/HP presets in config.h
- **ATTACKMODE**: `ATTACKMODE HID STORAGE` → composite USB (keyboard + mass storage)
  - MSC is gated behind the `ENABLE_MSC` build flag because the read/write callbacks
    are stubs (zeros on read, discard on write) — enabling MSC without raw SD I/O
    would expose a phantom drive to the host. Wire `SD_MMC.readRAW/writeRAW`
    before defining `ENABLE_MSC`.
- **Exfiltration**: `EXFIL_START` / `EXFIL_STOP` → captures outgoing keystrokes to /loot/keylog.bin
  - Capture buffer auto-flushes to SD on overflow (no silent drop)

## Key decisions
- Skip LCD for initial build (demo eye-candy only, adds failure surface)
- Static arrays in interpreter (no malloc — embedded safe)
- Auto-fire payload on USB plug-in (2s enumeration delay, configurable)
- WiFi C2: 192.168.4.1, SSID `DuckC2`, pass `quackquack` — change before any non-airgapped test
- C2 auth: 16-char token generated at boot, printed to USB-CDC serial; required on
  `X-Auth-Token` header for `PUT /api/payload/*`, `POST /api/run/*`, `POST /api/stop`
- VID/PID: Logitech 0x046D/0xC52B via build flags (baked into TinyUSB descriptor at compile time)
- OS detection: polling-based (keyboard.getLEDsStatus()) — no TinyUSB callback hooks needed

## Implementation roadmap (remaining)
- Phase 2.5: Port interpreter to CircuitPython for RP2350-One fallback
- Phase 3: BLE HID (ESP32-S3 BLE keyboard — "cableless Ducky")
- Phase 5: Defense tools (USBGuard rules, DuckHunt speed detector)
- Runtime VID/PID hot-swap (requires USB re-enumeration or core patch)
- Lock-LED covert channel (bidirectional, needs host-side component)
- More keyboard layouts (FR, ES, IT, Nordic)
- LCD: payload progress (PC/total), live OS/layout, current DuckyScript line

## Build
```bash
cd firmware && pio run --environment T-Dongle-S3 --target upload
```

If `dongles3` board def missing, copy from:
`https://github.com/Xinyuan-LilyGO/T-Dongle-S3/blob/main/boards/`

## Testing notes
- Lab VMs only (KVM qcow2 targets)
- UAC/sudo elevators are the brittlest part of payloads
- Timing: host needs ~1-2s after USB enumeration before accepting input
- Keyboard layout: default US breaks on PL/DE/etc. — watch for garbage chars

## Gotchas
- BACKSPACE scancode is 0x2a, ESC is 0x29
- `_server` naming conflict with `WebServer` class — instance is `_server`
- `WebServer::setInterpreter()` must be called after `Interpreter` construction
- SD card 1-bit mode needs INPUT_PULLUP on all 3 pins before `SD_MMC.begin()`
- Interpreter uses static char arrays (512 lines x 256 bytes) — no heap alloc
- VID/PID are compile-time only — change platformio.ini build flags, rebuild
- `loadBuffer()` resets all vars → call `setBuiltinVar()` AFTER loadBuffer to inject $_OS etc.
- MSC enumeration is opt-in via `-DENABLE_MSC`; without raw SD I/O it would expose a phantom drive
- OS detection heuristic may return UNKNOWN on hosts with minimal LED traffic
- `keyboard.getLEDsStatus()` availability depends on Arduino-ESP32 core version
- DE layout table covers common chars only — some edge cases may produce wrong keys
- `tick()` yields whenever `execLine()` returns false (CALL, RESTART_PAYLOAD, IF/ELSE_IF
  skip landings, cooperative DETECT_OS) — landing-on lines re-execute on the next tick
- `skipToMatching()` lands ON the matching `END_*` so the pop fires there; `skipToNext()`
  lands on the next ELSE_IF/ELSE/END_IF at the same depth. Both return false from
  `handleBlockStart` so `tick()` doesn't advance past the landed-on line.
- LCD backlight pin: LilyGO's official config is GPIO 37; if your board ships dark
  try GPIO 38 (some batches). Change both `PIN_LCD_BL` in config.h and `TFT_BL=`
  in platformio.ini together.
- LCD paints are diff-gated against a cached snapshot in display.cpp — paint cost
  is near-zero when fields are unchanged, so `update()` can be called every loop().
- T-Dongle-S3 ST7735 needs `TFT_INVERSION_ON=1` and `TFT_RGB_ORDER=TFT_BGR`. Without
  these the panel boots looking inverted or with wrong colours.
