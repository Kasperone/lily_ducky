# lily_ducky — DIY BadUSB on the LILYGO T-Dongle family

Research doc: `/home/kasperone/Documents/vault/Claude Code Research/DIY-Rubber-Ducky-BadUSB-Deep-Research.md`

**Before touching hardware-facing code, read
[`docs/knowledge-base/agent-playbook.md`](docs/knowledge-base/agent-playbook.md)
and [`docs/knowledge-base/open-questions.md`](docs/knowledge-base/open-questions.md).**
The gotchas below are the compressed facts; the knowledge base has the sourced
evidence behind them and — importantly — flags a few claims in this very file
that turned out to be imprecise or unverified when checked against a primary
source (register headers, vendor firmware, the full text of cited issues).

## Status
Phase 1-2 code written (interpreter + C2 + HAL). **Ported to T-Dongle-C5 (hardware
on hand) with T-Dongle-S3 kept as the full-HID reference target.** Both envs build.
**The C5 has been flashed and boot-verified on real hardware**: boot log, SD card,
LCD dashboard, and the BOOT button are all confirmed working on-device; the WiFi C2
SoftAP/HTTP server starts (confirmed via serial) but its REST API hasn't been
exercised end-to-end yet (needs a WiFi-joined client). The status LED is a
**confirmed dead/stuck hardware fault on this unit**, not a firmware bug — two
unrelated firmware images (vendor factory code and a from-scratch static-frame
diagnostic) both produced the same frozen, blended output regardless of commanded
colour; don't spend further effort on `sendAPA102()` or `usb_jtag_bridge_en` for
this symptom. See `docs/knowledge-base/open-questions.md` #1 and README's status
table for the full diagnostic trail.
T-Dongle-S3 remains compile-verified only — no hardware acquired.
Phase 4 security bypass features implemented (OS detect, layouts, VID/PID, jitter, exfil, ATTACKMODE) — S3 target only, unverified on hardware (no S3 board).

## ⚠️ The ESP32-C5 cannot be a USB keyboard
USB HID/MSC require a USB-OTG peripheral; in the ESP32 family only the S2/S3 have one.
Two independent reasons block it on the C5, either one sufficient:
1. **Silicon** — the C5's peripheral list is a *fixed-function USB Serial/JTAG
   controller* (CDC-ACM + JTAG, hard-wired) with no USB-OTG: datasheet §4.2.1.5, and
   ESP-IDF `soc_caps.h` defines `SOC_USB_SERIAL_JTAG_SUPPORTED=1` with no
   `SOC_USB_OTG_SUPPORTED`. Arduino's `USBHIDKeyboard` is gated on that macro, so it
   compiles to nothing on the C5.
2. **Software** — even under the reading that the C5 has latent OTG silicon,
   Espressif marked C5 TinyUSB device-mode "Won't Do" (esp-idf#18625): no HID/MSC
   stack ships, so there is no path at the driver layer either. (esp-usb#371 is a
   separate, narrower, *resolved* MSC build-error issue — don't cite it as a
   second "won't do" data point; see docs/knowledge-base/open-questions.md #3.)
Consequence: on the C5 build every HID function is an honest no-op; the device is a
WiFi C2 lab node + interpreter/display/storage exerciser. Keystroke injection needs the
T-Dongle-S3 (USB-OTG) — or BLE HID, which both boards can do (BLE 5).

## Architecture
- **Targets**: LILYGO T-Dongle-C5 (ESP32-C5, 16MB Flash, 8MB PSRAM, WiFi 6 dual-band)
  and T-Dongle-S3 (ESP32-S3, 16MB Flash, USB-OTG HID)
- **Target selection**: `-DTARGET_DONGLE_C5` / `-DTARGET_DONGLE_S3` in platformio.ini →
  all pin + capability differences resolve in `firmware/src/config.h` via `CFG_HAS_USB_HID`.
  No other file may contain target-specific `#ifdef TARGET_*`.
- **Lang**: C/C++ Arduino framework (Arduino-ESP32 core 3.x via **pioarduino fork** —
  official platformio/espressif32 has no ESP32-C5 support)
- **USB HID**: `USBHIDKeyboard` (built into Arduino-ESP32 core) — S3 only
- **WiFi C2**: SoftAP + built-in `WebServer` (dashboard + REST API) — both targets
- **Storage**: T-Dongle-C5: SD over SPI (SCK=6, MISO=7, MOSI=2, CS=23; shared bus with
  LCD). T-Dongle-S3: `SD_MMC` 1-bit mode (CLK=12, CMD=16, D0=14). Both behind `Storage::`
  API + `Storage::fs()` accessor.
- **LCD**: ST7735 0.96" 80×160 IPS via SPI — TFT_eSPI, `LCD_ENABLED=1` in config.h.
  Landscape (160×80), shows LilyDucky title, SSID, IP, auth token, client count, status corner
- **LED**: APA102, bit-banged in hal.cpp (no library) — C5: DI=5 CI=4; S3: DIN=40 CLK=39
- **Button**: C5: GPIO28 (BOOT); S3: GPIO0
- **Board definitions**: vendored in `firmware/boards/` (Lilygo-T-Dongle-C5.json, dongles3.json)
- **External libs**: `bodmer/TFT_eSPI` only (single dep; chosen over hand-rolled ST7735
  init for board-validated correctness). Everything else ships with Arduino-ESP32 core

## T-Dongle-C5 pin map (from LilyGO factory firmware pin_config.h)
| Function | GPIO | Function | GPIO |
|---|---|---|---|
| LCD MOSI / SD CMD | 2 | LCD SCK / SD CLK | 6 |
| LCD MISO / SD D0 | 7 | LCD CS | 10 |
| LCD DC (RS) | 3 | LCD RST | 1 |
| LCD BL | 0 | SD CS | 23 |
| LED DI | 5 | LED CI | 4 |
| BOOT button | 28 | USB D-/D+ (Serial/JTAG) | 13/14 |

## Security bypass features (DuckyScript commands)
- **OS Detection**: `DETECT_OS` → sets $_OS (WINDOWS=1, MACOS=2, LINUX=3), $_HOST_CONFIGURATION_REQUEST_COUNT
  - Auto-runs at boot before payload fires; also callable mid-payload
  - Polls keyboard LED status register during 3s enumeration window
  - Cooperative: the interpreter yields to `loop()` (and therefore `WebServer::tick()`) during the wait
  - C5: returns OS_UNKNOWN immediately (no HID to observe)
- **Keyboard Layouts**: `LAYOUT US|PL|DE` → switches ASCII-to-HID scancode mapping
  - PL (Programmer's) = US base + AltGr diacritics
  - DE (QWERTZ) = Y/Z swap, AltGr special chars, shifted punctuation
- **Jitter**: `JITTER_MAX <ms>` → random 0-N ms delay between keystrokes (human-like timing)
- **VID/PID Spoofing**: compile-time via platformio.ini (`-DUSB_VID=0x046D -DUSB_PID=0xC52B`, S3 env only)
  - Logitech Unifying Receiver preset; Dell/HP presets in config.h
  - C5's Serial/JTAG identity (Espressif 0x303A) is fixed in silicon
- **ATTACKMODE**: `ATTACKMODE HID STORAGE` → composite USB (keyboard + mass storage)
  - MSC is gated behind the `ENABLE_MSC` build flag because the read/write callbacks
    are stubs (zeros on read, discard on write) — enabling MSC without raw SD I/O
    would expose a phantom drive to the host. Wire `SD.readRAW/writeRAW`
    before defining `ENABLE_MSC`.
- **Exfiltration**: `EXFIL_START` / `EXFIL_STOP` → captures outgoing keystrokes to /loot/keylog.bin
  - Capture buffer auto-flushes to SD on overflow (no silent drop)

## Key decisions
- Skip LCD for initial build (demo eye-candy only, adds failure surface) — LCD code exists but optional
- Static arrays in interpreter (no malloc — embedded safe)
- Auto-fire payload on USB plug-in (2s enumeration delay, configurable) — S3 only;
  C5 boots straight to WiFi C2 and runs the payload with typing as no-ops
- WiFi C2: 192.168.4.1, SSID `LilyC2`, pass `quackquack` — change before any non-airgapped test
- C2 auth: 16-char token generated at boot, printed to USB-CDC serial; required on
  `X-Auth-Token` header for `PUT /api/payload/*`, `POST /api/run/*`, `POST /api/stop`
- VID/PID: Logitech 0x046D/0xC52B via build flags (baked into TinyUSB descriptor at compile time)
- OS detection: polling-based (keyboard.getLEDsStatus()) — no TinyUSB callback hooks needed
- Dual-target strategy: C5 = lab node on hand; S3 = the actual BadUSB. Single source tree,
  capability flag, no code forks/branches per board.

## Implementation roadmap (remaining)
- **Next phase — build verification VM**: install PlatformIO there, build both envs,
  flash the C5 (this machine only hosts the repo + GitHub pushes)
- Phase 2.5: Port interpreter to CircuitPython for RP2350-One fallback
- Phase 3: BLE HID ("cableless ducky") — both boards have BLE 5, so it runs on either;
  it is the **C5's only keystroke-injection route** since the C5 has no USB HID
- Phase 5: Defense tools (USBGuard rules, DuckHunt speed detector)
- Runtime VID/PID hot-swap (requires USB re-enumeration or core patch) — S3 only
- Lock-LED covert channel (bidirectional, needs host-side component)
- More keyboard layouts (FR, ES, IT, Nordic)
- LCD: payload progress (PC/total), live OS/layout, current DuckyScript line

## Build (runs on the build VM, not this machine)
```bash
cd firmware
pio run --environment T-Dongle-C5            # board on hand (default env)
pio run --environment T-Dongle-S3            # full HID target
pio run --environment T-Dongle-C5 --target upload
```

Platform note: platformio.ini uses the **pioarduino fork** release URL (55.03.311) —
official platformio/espressif32 doesn't support ESP32-C5. Board JSONs are vendored in
`firmware/boards/` (from LilyGO repos) so no manual copy into ~/.platformio is needed.
C5 upload: hold BOOT (GPIO28) while plugging in if the port isn't detected.

## Git conventions (see CONTRIBUTING.md)
- Branches: `hw/<board>`, `feat/<name>`, `fix/<name>`, `docs/<name>`, `refactor/<name>` off `main`
- Conventional Commits, imperative mood; body explains the WHY/constraint with sources
- One logical change per commit; build both envs when touching shared code
- No secrets in history. Tag `v0.x.y` milestones when demonstrable on hardware.
- Code style: `docs/programming-style.md` (Torvalds-inspired; data structures first,
  earn abstractions, comments = WHY only)

## Testing notes
- Lab VMs only (KVM qcow2 targets)
- UAC/sudo elevators are the brittlest part of payloads
- Timing: host needs ~1-2s after USB enumeration before accepting input
- Keyboard layout: default US breaks on PL/DE/etc. — watch for garbage chars

## Gotchas
- **ESP32-C5 has NO USB-OTG** — never plan HID/MSC/VID-PID features for it; check
  `CFG_HAS_USB_HID` before writing USB-dependent code
- BACKSPACE scancode is 0x2a, ESC is 0x29
- `_server` naming conflict with `WebServer` class — instance is `_server`
- `WebServer::setInterpreter()` must be called after `Interpreter` construction
- S3 SD card 1-bit mode needs INPUT_PULLUP on all 3 pins before `SD_MMC.begin()`
- C5 SD card is SPI mode, shares the bus with the LCD (call `SPI.begin(sck, miso, mosi, cs)`
  with the SD pins before `SD.begin(CS)`)
- C5 BOOT button (GPIO28) held at plug-in = download mode; also our user button —
  don't treat boot-mode entry as a bug
- Interpreter uses static char arrays (512 lines x 256 bytes) — no heap alloc
- VID/PID are compile-time only — change platformio.ini build flags, rebuild (S3 env)
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
- LCD backlight pin differs per board: C5 GPIO0, S3 GPIO37 (some S3 batches 38).
  Change `PIN_LCD_BL` in config.h and `TFT_BL=` in platformio.ini together.
- LCD paints are diff-gated against a cached snapshot in display.cpp — paint cost
  is near-zero when fields are unchanged, so `update()` can be called every loop().
- ST7735 on both boards needs `TFT_INVERSION_ON=1` and `TFT_RGB_ORDER=TFT_BGR`.
- `cardType()`/`cardSize()` live on SDFS/SDMMCFS, not fs::FS — use Storage::cardSize()
  helpers, don't call them through `Storage::fs()`.
- LSP/clang errors about missing Arduino.h in the editor are expected (no toolchain
  headers outside PlatformIO) — PlatformIO build is the source of truth.
