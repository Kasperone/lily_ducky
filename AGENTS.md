# lily_ducky — DIY BadUSB on the LILYGO T-Dongle family

**Private, vault-authored notes** (research docs, machine/VM facts, USB
passthrough, host udev automation, local paths) live ONLY in the Obsidian
vault — never in this public repo. Each clone links them in with a *gitignored*
`Claude-Docs/` symlink via `scripts/link-claude-docs.sh` (run once per machine;
`VAULT=... ` to override the vault path). Once linked, they are readable at
`Claude-Docs/` from the repo root — e.g. the deep-research doc is
`Claude-Docs/DIY-Rubber-Ducky-BadUSB-Deep-Research.md`, and `CLAUDE.md` imports
`Claude-Docs/env.md`. If `Claude-Docs/` is missing, run the script; if the
vault itself isn't synced here, that's a per-machine setup step, not a repo bug.
Keep private facts in the vault and only source-backed, public-safe knowledge in
this file and `docs/knowledge-base/`.

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
SoftAP/HTTP server starts and its **REST API is now verified end-to-end on
hardware** (2026-08-30) — 12/12 checks via an on-device loopback self-test
(`firmware/src/c2/c2_selftest.cpp`, env `T-Dongle-C5-selftest`, results over
serial): status, authenticated payload PUT + GET round-trip, listing, 401 on
missing token, run-to-completion, mid-run stop, and DETECT_OS cooperative pause
with the C2 staying responsive (`$_OS=0` on the C5). That test **caught and
fixed a real bug**: the parameterized routes were registered as bare strings
(`"/api/payload/(.*)"`), which the Arduino `WebServer` matches *literally*
(`Uri::canHandle` is `_uri == requestUri`), so every real filename 404'd —
payload upload/run had never worked from any client. Fixed with
`UriBraces("/api/payload/{}")` (see the gotcha below). Still pending: a true
**over-the-air** pass with `scripts/c2_api_test.sh` from a WiFi-joined client —
the build/flash VM has no WiFi radio, so it can't join the SoftAP; loopback
covers the HTTP+handler+interpreter+SD path but not the radio/DHCP path. (Also
noted: own-AP-IP loopback to 192.168.4.1 isn't routed on this lwIP build; the
self-test reaches the server via 127.0.0.1 — a loopback detail, not a bug.) The status LED **works** — RESOLVED 2026-08-30 after ~15 sessions of a
"dead LED" red herring. It was a **pin bug**: the APA102 is on **GPIO2 (data)
/ GPIO6 (clock)** — the LCD/SD SPI bus — **not GPIO4/5** as the vendor's own
`pin_config.h` claims. Every prior test drove 4/5 (the JTAG pads), which
never reaches the LED; the "solid white/amber" everyone saw was the LED
holding stale data on the shared bus. Proven with an A/B pin test on hardware
(`firmware/src/diag/led_pinmap_diag.cpp`: colours cycle on 2/6, frozen on
5/4) and fixed in the firmware (`CFG_LED_SHARED_SPI` — the LED is driven over
the shared `SPI` bus and re-latched after LCD/SD traffic). Steady teal status
LED + LCD dashboard both verified working simultaneously; LCD backlight
restored (`CFG_LCD_BL_LEVEL` 180). `usb_jtag_bridge_en` /
`CFG_RELEASE_JTAG_LED_PINS` are now moot for the LED and left dead-0. The
source that cracked it: `github.com/zombodotcom/T-Dongle-C5`. Full trail:
`docs/knowledge-base/open-questions.md` #1. T-Dongle-S3 remains
compile-verified only — no hardware acquired (its LED is on dedicated pins
40/39, bit-banged, unverified).
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
- **LED**: APA102 in hal.cpp (no library). C5: DI=2/CI=6 — on the SHARED LCD/SD
  SPI bus (not 4/5; vendor pin_config.h is wrong — see open-questions.md #1),
  driven over `SPI` (`CFG_LED_SHARED_SPI`) + re-latched after bus traffic.
  S3: DIN=40 CLK=39, dedicated pins, bit-banged.
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
| LED DI (=LCD MOSI) | 2 | LED CI (=LCD SCK) | 6 |
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
- WiFi C2: 192.168.4.1, SSID `LilyC2`, password set in gitignored `firmware/src/config_secret.h` (copy from `config_secret.h.example`) — not committed
- C2 auth: 16-char token generated at boot, printed to USB-CDC serial; required on
  `X-Auth-Token` header for `PUT /api/payload/*`, `POST /api/run/*`, `POST /api/stop`
- VID/PID: Logitech 0x046D/0xC52B via build flags (baked into TinyUSB descriptor at compile time)
- OS detection: polling-based (keyboard.getLEDsStatus()) — no TinyUSB callback hooks needed
- Dual-target strategy: C5 = lab node on hand; S3 = the actual BadUSB. Single source tree,
  capability flag, no code forks/branches per board.

## Implementation roadmap (remaining)
- ~~Build verification VM~~ — done: PlatformIO is installed on the hardware VM;
  both envs build green and the C5 flashes directly here.
- Phase 2.5: Port interpreter to CircuitPython for RP2350-One fallback
- Phase 3: BLE HID ("cableless ducky") — both boards have BLE 5, so it runs on either;
  it is the **C5's only keystroke-injection route** since the C5 has no USB HID
- Phase 5: Defense tools (USBGuard rules, DuckHunt speed detector)
- Runtime VID/PID hot-swap (requires USB re-enumeration or core patch) — S3 only
- Lock-LED covert channel (bidirectional, needs host-side component)
- More keyboard layouts (FR, ES, IT, Nordic)
- LCD: payload progress (PC/total), live OS/layout, current DuckyScript line

## Build (PlatformIO 6.1.19 is installed on the hardware VM — build + flash here)

> The T-Dongle-C5 is USB-passthrough'd into this VM (`/dev/ttyACM0`,
> `303a:1001`) and PlatformIO is installed (`~/.local/bin/pio`), so builds and
> flashes both run here — there is no separate build-only machine. (An earlier
> version of this file said "runs on the build VM, not this machine"; that's
> obsolete for the current hardware VM. `esptool` ships inside PlatformIO's
> toolchain; `idf.py`/standalone `esptool` are NOT installed, so native ESP-IDF
> examples can't be flashed directly without setting up ESP-IDF.)
```bash
cd firmware
pio run --environment T-Dongle-C5            # board on hand (default env)
pio run --environment T-Dongle-S3            # full HID target
pio run --environment T-Dongle-C5 --target upload
# C2 REST API end-to-end self-test on hardware (no WiFi client needed):
pio run -e T-Dongle-C5-selftest -t upload    # then capture serial for [PASS]/[FAIL]
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
- **C2 routes with a path arg MUST use `UriBraces("/api/x/{}")`, never a bare
  `"/api/x/(.*)"` string** — Arduino's `WebServer` matches a plain-String route
  *literally* (`Uri::canHandle` is `_uri == requestUri`), so `(.*)` matched only
  the literal text and every real filename 404'd. `{}` captures the segment into
  `pathArg(0)` and forbids `/` (which `validName` already rejects). Regression-
  guarded by the C2 loopback self-test (`env T-Dongle-C5-selftest`).
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
