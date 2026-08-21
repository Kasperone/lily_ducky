# LilyDucky

> A DIY USB Rubber Ducky / BadUSB learning platform on the LILYGO T-Dongle family.
> Built to understand **how** keystroke-injection attacks work — so you can defend against them.

> ### 🚧 Early stage — untested code
> This project is at its **starting point**: the firmware has been written and
> ported but **never compiled, flashed, or run on real hardware**. Expect build
> errors and bugs. Treat everything here as a work-in-progress learning codebase,
> not working software — and verify it all yourself before trusting it.

---

## ⚖️ Read this first — legal & safety

**This project is for education and authorized security research only.**

A keystroke-injection device is functionally identical to the tooling used in real attacks.
Owning one is legal in most places; **using it on systems you don't own or don't have
written authorization to test is a crime** — unauthorized computer access laws apply
regardless of intent, and "it was just a demo" is not a defense.

| Rule | Why |
|---|---|
| 🎯 **Your own hardware only** | Test exclusively against machines you own — lab VMs, spare laptops. Never plug into employer, public, or borrowed systems. |
| 📄 **Written authorization** | Any test on someone else's equipment requires explicit written permission first. |
| 📦 **Physical security** | Treat the flashed dongle like a loaded weapon: label it, store it, never leave it unattended where someone could plug it in. |
| 🔒 **Change default credentials** | The WiFi C2 defaults (`LilyC2` / `quackquack`) are public. Change them in `firmware/src/config.h` before any test that isn't fully air-gapped. |
| 🧹 **Wipe before disposal** | The SD card stores payloads and captured keystrokes (`/loot`). Wipe it before selling, lending, or trashing the device. |

**Jurisdiction matters.** Some countries restrict possession or import of "hacker tools".
Know your local law (e.g. EU: check national implementation of Directive 2013/40/EU;
US: CFAA 18 U.S.C. §1030; UK: Computer Misuse Act 1990) before building or carrying this.

By using this repository you accept that the authors are not responsible for misuse.

---

## 🧭 What this project is

LilyDucky emulates a USB keyboard and executes **DuckyScript** payloads (the language of
the Hak5 Rubber Ducky) at superhuman speed. It also runs a **WiFi C2 dashboard** to
control the device remotely. The goal is learning by building: USB enumeration, HID
descriptors, interpreters, embedded web servers — each layer is small, commented, and
deliberately readable.

### Two hardware targets

| | **T-Dongle-C5** (ESP32-C5) | **T-Dongle-S3** (ESP32-S3) |
|---|---|---|
| Status | ✅ Hardware on hand — primary build target | 🔜 Reference target (not yet acquired) |
| USB HID keyboard | ❌ **Impossible in silicon** — no USB-OTG | ✅ Full BadUSB functionality |
| WiFi | WiFi 6, 2.4 + 5 GHz | WiFi 4 (2.4 GHz) |
| LCD / LED / SD | ✅ all | ✅ all |
| Role in this repo | C2 lab node: dashboard, interpreter, display | The actual keystroke-injection device |

> **Why can't the C5 type?** USB HID needs a **USB-OTG** peripheral — and in the ESP32
> family only the **S2/S3** have one. The C5 (like the C3/C6/H2) ships only a
> *fixed-function USB Serial/JTAG controller*: hard-wired as a CDC-ACM serial + JTAG
> console, it can never enumerate as a keyboard. Confirmed by the C5 datasheet
> (§4.2.1.5 lists only the Serial/JTAG controller — no OTG) and by Arduino-ESP32 gating
> `USBHIDKeyboard` on `SOC_USB_OTG_SUPPORTED`, which is `0` on the C5. So the C5 build
> runs the full interpreter with typing as a no-op — every non-USB subsystem (LCD, LED,
> SD, WiFi C2, payload logic) is still fully exercised.

---

## 🚀 Quick start

### Prerequisites

- [PlatformIO Core CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
  (or the VS Code extension)
- A USB cable that carries **data**, not just power
- A micro SD card (FAT32)

### 1. Build & flash (T-Dongle-C5)

```bash
cd firmware
pio run --environment T-Dongle-C5 --target upload
```

> If the upload port isn't detected: hold the **BOOT** button while plugging the dongle
> in (download mode), then retry. On Linux you may need udev rules or `dialout` group
> membership for `/dev/ttyACM0`.

### 2. Watch the boot log

```bash
pio device monitor
```

You'll see HAL/SD/WiFi init, and the **C2 auth token** printed once — save it.

### 3. Load a payload

Copy a `.dd` file to the SD card as `/payloads/payload.dd` (or any name), insert it,
plug the dongle in. On the C5 the payload parses and runs with typing disabled; on the
S3 it types into the host.

### 4. Use the WiFi dashboard (both boards)

1. Connect to the AP: SSID `LilyC2` (change it! see safety box above)
2. Open `http://192.168.4.1` in a browser
3. Enter the auth token from the serial console when prompted
4. Edit / upload / run / stop payloads from the browser — no SD card reader needed

**REST API** (mutating routes need `X-Auth-Token`):

```
GET  /api/status           → {"state":"idle","clients":0,"ap_ip":"192.168.4.1"}
GET  /api/payloads         → [{"name":"hello.dd","size":63}]
GET  /api/payload/<name>   → payload text
PUT  /api/payload/<name>   → save body as payload          [auth]
POST /api/run/<name>       → execute payload               [auth]
POST /api/stop             → stop running payload          [auth]
```

---

## 🗂 Project structure

```
firmware/                  PlatformIO project
  platformio.ini           Build config (both targets, pioarduino platform fork)
  boards/                  Vendored LilyGO board definitions
    Lilygo-T-Dongle-C5.json
    dongles3.json
  partitions.csv           S3 flash layout (16 MB)
  src/
    config.h               ← all target differences resolve here
    main.cpp               Boot sequence + state wiring
    hal/                   Hardware abstraction (USB HID, APA102 LED, button)
    interpreter/           DuckyScript parser & executor (Tier 1 + Tier 2)
    c2/                    WiFi SoftAP dashboard + REST API
    display/               APA102 status colours + ST7735 LCD frame
    storage/               SD card (SDMMC on S3, SPI on C5) behind one API
payloads/                  Sample .dd scripts for testing
docs/                      Contributor docs (programming style)
```

## 🦆 DuckyScript support

**Tier 1** — `REM`, `STRING`, `STRINGLN`, `DELAY`, named keys (`ENTER`, `TAB`, `ESCAPE`,
arrows…), modifiers (`GUI`, `CTRL`, `ALT`, `SHIFT`), `F1`–`F12`, combos (`CTRL ALT DELETE`).

**Tier 2** — `DEFINE`, `VAR`, `IF/ELSE_IF/ELSE/END_IF`, `WHILE/END_WHILE`,
`FUNCTION/CALL/END_FUNCTION`, `RESTART_PAYLOAD`, `STOP_PAYLOAD`, `DEFAULTDELAY`.

**Security-bypass extensions** (S3 only — each rides the USB HID path: descriptor
spoofing, keystroke output, or reading the keyboard LED-status register, all of which
need USB-OTG) — `DETECT_OS` → `$_OS`,
`LAYOUT US|PL|DE`, `JITTER_MAX`, `EXFIL_START/STOP`, `ATTACKMODE HID STORAGE`,
compile-time VID/PID spoofing (see `firmware/platformio.ini`).

## 🔧 Development

```bash
cd firmware
pio run --environment T-Dongle-C5        # build only
pio run --environment T-Dongle-S3        # build the S3 target
pio run --environment T-Dongle-C5 --target clean && pio run -e T-Dongle-C5
```

Before you contribute, read:

- **[CONTRIBUTING.md](CONTRIBUTING.md)** — git branch & commit conventions (agents welcome)
- **[docs/programming-style.md](docs/programming-style.md)** — the code style we follow
- **[AGENTS.md](AGENTS.md)** — architecture decisions & hardware gotchas for AI agents

## 📚 Further reading

- Research notes that started this project:
  `~/Documents/vault/Claude Code Research/DIY-Rubber-Ducky-BadUSB-Deep-Research.md`
- [Hak5 DuckyScript docs](https://docs.hak5.org/hak5-usb-rubber-ducky/duckyscript-3-overview)
- [USB HID spec — keyboard boot report](https://usb.org/document-library/device-class-definition-hid-111)
- Defense counterparts: **USBGuard** (Linux), **DuckHunt** (keystroke-speed detection)

## 📜 License

MIT — see [LICENSE](LICENSE). Educational use only; see the legal box at the top.
