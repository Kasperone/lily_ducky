# ESP32-C5 hardware reference

Primary chip on the T-Dongle-C5 board. This file is the sourced backing for the
capability claims in `AGENTS.md`.

## USB capability — no supported device-mode software path

**Claim:** the C5 cannot be a USB HID keyboard or USB mass-storage device, on any
software stack Espressif currently ships. Status: `VERIFIED (source)`.

Two separate things are true and worth not conflating:

1. **ESP-IDF's own capability header** (`soc_caps.h` for esp32c5) defines
   `SOC_USB_SERIAL_JTAG_SUPPORTED=1` and defines no `SOC_USB_OTG_SUPPORTED` /
   DWC2 capability at all. Arduino-ESP32's `USBHIDKeyboard` and TinyUSB device
   mode are both gated on that macro, so they compile to nothing on the C5
   regardless of what the silicon can physically do.
2. **Whether the silicon itself has latent USB-OTG hardware is disputed upstream,
   and irrelevant in practice.** Espressif's own product page advertises a
   "USB 2.0 OTG Full-Speed (12 Mbps) peripheral" on the C5, and
   [espressif/esp-idf#18625](https://github.com/espressif/esp-idf/issues/18625)
   is exactly a developer pointing out that gap — no `SOC_USB_OTG_*` cap, no HAL
   glue, so the datasheet's marketing claim has no driver behind it. That issue
   carries both `Resolution: Won't Do` and `Status: Done` labels — Espressif is
   not going to add TinyUSB device-mode support for the C5.

   Do **not** also cite `espressif/esp-usb#371` for this — it's a different,
   narrower issue (a TinyUSB **MSC** build error on an IDF 6.0-beta1 snapshot)
   closed `Resolution: Done` / `Status: Done`, i.e. *fixed*, not *won't-fix*. It
   does not support the "permanently blocked" claim and citing it alongside
   `#18625` overstates the case. `AGENTS.md` currently pairs them — see
   `open-questions.md` #3.

**Practical consequence** (unaffected by the dispute above): on the C5 build,
`CFG_HAS_USB_HID=0`, every keystroke/typing path is a no-op, and the device is a
WiFi C2 node + interpreter/display/storage exerciser. Keystroke injection needs the
T-Dongle-S3 (real USB-OTG, TinyUSB HID) or BLE HID (works on either board, not yet
implemented — see `AGENTS.md` roadmap).

**Recheck trigger:** if a future ESP-IDF release adds `SOC_USB_OTG_SUPPORTED` for
esp32c5, this entire conclusion needs revisiting. Check
`components/soc/esp32c5/include/soc/soc_caps.h` in a current `esp-idf` checkout.

## USB Serial/JTAG controller and pin sharing

The C5's `USB_SERIAL_JTAG` controller is a fixed-function CDC-ACM + JTAG console
(datasheet §4.2.1.5). It occupies GPIO13/14 (D-/D+) for the USB PHY. It can
*also* drive four more pins as an internal JTAG TAP:

| JTAG signal | GPIO | Direction |
|---|---|---|
| MTMS | GPIO2 | into chip |
| MTDI | GPIO3 | into chip |
| MTCK | GPIO4 | into chip (clock) |
| MTDO | GPIO5 | out of chip |

Source: ESP32-C5 datasheet strapping-pin table (pin 11=MTMS/GPIO2, pin 12=MTDI/GPIO3,
pin 13=MTCK/GPIO4, pin 15=MTDO/GPIO5) and the ESP-IDF "Configure Other JTAG
Interfaces" guide for esp32c5, which independently gives the same GPIO2–5 mapping.
`VERIFIED (source)`, cross-checked against two independent Espressif documents.

**T-Dongle-C5 wiring collision:** LilyGO put the APA102 status LED on GPIO4 (clock)
and GPIO5 (data) — i.e. on MTCK and MTDO. Confirmed directly from the vendor's own
firmware repo, `Xinyuan-LilyGO/T-Dongle-C5`, `include/pin_config.h`:

```c
#define LED_CI_PIN 4
#define LED_DI_PIN 5
```

`AGENTS.md` currently calls these "MTCK/MTDI" — GPIO5 is MTDO, not MTDI. Minor
naming error, doesn't change the pin numbers or the fix, but fix the label if you
touch that section (see `open-questions.md` #5).

### The `usb_jtag_bridge_en` register — background, now secondary to a confirmed hardware fault

This is the register `firmware/src/hal/hal.cpp` sets to `1` at boot, believing
it "releases" GPIO4/5 from JTAG so the LED can drive them. **The LED on this
board's specific unit is now confirmed dead by direct hardware testing — see
`open-questions.md` #1.** Two structurally unrelated firmware images (vendor
factory code, and a from-scratch diagnostic sending one static frame per colour
with nothing else running) both produced the same frozen, blended, unresponsive
output — a signature no firmware bug can produce. The register-direction
question below is still an interesting, unreconciled documentation puzzle, but
it no longer has practical bearing on this board: whichever direction is
"correct," the LED can't confirm or refute it while it's physically stuck. The
primary-source facts, kept here for whoever eventually has a second unit to
test on:

From `esp-idf`'s `components/soc/esp32c5/register/soc/usb_serial_jtag_struct.h`
(`conf0` register, bit 15):

```
usb_jtag_bridge_en : R/W; bitpos: [15]; default: 0;
  Set this bit usb_jtag, the connection between usb_jtag and internal JTAG is
  disconnected, and MTMS, MTDI, MTCK are output through GPIO Matrix, MTDO is
  input through GPIO Matrix.
```

Default is **0**. The Espressif guide this register belongs to is titled
"Configure Other JTAG Interfaces" and is about wiring an *external* JTAG probe to
GPIO2–5 while still tunneling through the onboard USB port — a debug-bring-up
feature, not a "give me my GPIOs back" feature. Setting the bit is documented as
routing signals *through* the GPIO matrix *to* the pins for that external-probe
case — the opposite of freeing them for LED use.

**Vendor counter-evidence:** LilyGO's own factory firmware
(`examples/Factory/Factory.ino`, `examples/LED/led.ino` in `Xinyuan-LilyGO/T-Dongle-C5`)
drives GPIO4/5 with plain `pinMode(..., OUTPUT)` / `digitalWrite()` via the Pololu
APA102 Arduino library — **no register poke at all** — and ships this as their
default retail demo. That's strong evidence GPIO4/5 already work as plain GPIO with
the register at its default (0).

## Strapping pins

Strapping pins (sampled at reset/power-on, then free for GPIO use): `GPIO7`,
`GPIO25`, `GPIO26`, `GPIO27`, `GPIO28`, plus `MTMS` (GPIO2) and `MTDI` (GPIO3).
`GPIO28` is also this board's BOOT button — holding it at power-on enters download
mode; this is expected, not a bug (already noted in `AGENTS.md`).

## Vendor pinout — full cross-check

From `Xinyuan-LilyGO/T-Dongle-C5` `include/pin_config.h` (and duplicated in
`examples/Factory/pin_config.h`), which matches this repo's `firmware/src/config.h`
pin-for-pin:

```c
#define SPI_MOSI 2
#define SPI_MISO 7
#define SPI_SCK  6
#define PIN_LCD_MOSI  SPI_MOSI
#define PIN_LCD_SCK   SPI_SCK
#define PIN_LCD_BL    0
#define PIN_LCD_RST   1
#define PIN_LCD_DC    3
#define PIN_LCD_CS    10
#define LED_CI_PIN 4
#define LED_DI_PIN 5
#define BOOT_BTN 28
#define SD_CMD_PIN SPI_MOSI
#define SD_DAT0_PIN SPI_MISO
#define SD_CLK_PIN SPI_SCK
#define SD_CS_PIN 23
#define UART0_TX_PIN 11
#define UART0_RX_PIN 12
```

`VERIFIED (source)` — this repo's `config.h` pin map matches the vendor file
exactly. If a future pin-map bug is suspected, this vendor file (not the LilyGO
wiki, which can lag) is the fastest ground truth to diff against.
