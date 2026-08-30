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

### The `usb_jtag_bridge_en` register — direction question RESOLVED 2026-08-30; still no fix for this unit's LED

This is the register `firmware/src/hal/hal.cpp` sets to `1` at boot, believing
it "releases" GPIO4/5 from JTAG so the LED can drive them. **That belief is
wrong, confirmed by the register's own full documentation text, not just an
inference — see below.** The LED's actual status is a longer story with two
retracted "confirmed dead" conclusions already; full arc, including a
2026-08-30 addendum that used a genuinely hardware-timed driver and both
`bridge_en` states, is in `open-questions.md` #1 — read it before summarizing
this any further.

Full register definition, from `esp-idf`'s
`components/soc/esp32c5/register/soc/usb_serial_jtag_struct.h` (`conf0`
union, bit 15 — re-fetched in full 2026-08-30; an earlier pass here quoted
only an excerpt):

```c
/** usb_jtag_bridge_en : R/W; bitpos: [15]; default: 0;
 *  Set this bit usb_jtag, the connection between usb_jtag and
 *  internal JTAG is disconnected, and MTMS, MTDI, MTCK output
 *  via GPIO Matrix, MTDO inputs via GPIO Matrix.
 */
uint32_t usb_jtag_bridge_en:1;
```

Default is **0**. Read literally and completely: setting this bit to 1 makes
MTMS/MTDI/MTCK (GPIO2/3/4) **outputs** and **MTDO (GPIO5) an input** — all
via the GPIO matrix. GPIO5 is `PIN_LED_DATA` on this board. **Setting this
bit therefore forces the LED's data pin into input direction** — no
`pinMode`/`digitalWrite`/`SPIClass` call on our side can drive it as output
while the bit is set, regardless of which driver generates the signal. This
is the opposite of "releasing" the pins for LED use; it's routing the
internal soft-JTAG-over-USB bridge *out* to physical GPIO2-5 so an
**external** JTAG probe can be wired there (matching this register's own
guide, "Configure Other JTAG Interfaces" — an external-probe bring-up
feature). GPIO4 (MTCK/`PIN_LED_CLOCK`) stays an output either way, which is
exactly why every bit-banged and SPI-based LED test that had this bit set
showed a live, toggling clock with data that could never actually change —
a clean mechanism, not a coincidence.

**The C5's "Configure Other JTAG Interfaces" guide adds the other half of
this**: JTAG is connected to the built-in USB_SERIAL_JTAG peripheral **by
default**, and only bridged onto physical GPIO2-5 when explicitly asked for
(this register bit, or burning the `DIS_USB_JTAG`/`JTAG_SEL_ENABLE` eFuses).
GPIO2-5 are not inherently "claimed" by JTAG at reset the way this section
used to assume — there was never anything to release.

**Vendor evidence, now explained rather than just observed:** LilyGO's own
factory firmware (`examples/Factory/Factory.ino`, `examples/LED/led.ino` in
`Xinyuan-LilyGO/T-Dongle-C5`) drives GPIO4/5 with plain `pinMode(...,
OUTPUT)` / `digitalWrite()` via the Pololu APA102 Arduino library — **no
register poke at all**. That isn't a lucky omission; per the two sources
above, GPIO4/5 are plain, fully usable GPIOs at the register's default (0),
and touching `usb_jtag_bridge_en` at all is actively counterproductive for
this use case.

**Practical status:** `CFG_RELEASE_JTAG_LED_PINS` in `config.h` sets this bit
and should be changed to not do so — flip it to 0 (or remove the register
poke) the next time someone has hardware available to verify the change
doesn't regress anything else. This was retested with the bit left
untouched entirely on 2026-08-30 (see `open-questions.md` #1 addendum): the
currently-attached unit's LED was still non-responsive, so this fix alone
does not resolve the LED — but it is still correct to make, independent of
that unit's condition, because the register direction is no longer in
dispute.

## SPI bus enum — only `FSPI` is valid on this chip, `HSPI` silently fails

`VERIFIED (source)`, discovered 2026-08-30 debugging an LED diagnostic (see
`open-questions.md` #1) — worth knowing for **any** future hardware-SPI code
on the C5, not just the LED. Arduino-ESP32's `cores/esp32/esp32-hal-spi.h`
defines the bus-select constants per chip family:

```c
// ESP32 (classic): FSPI=1, HSPI=2, VSPI=3 — three usable GP-SPI buses
// ESP32C2, C3, C5, C6, C61, H2: FSPI=0 only — one GP-SPI peripheral (SPI2)
// ESP32S2, S3, P4: FSPI=0, HSPI=1 — two GP-SPI peripherals (SPI2, SPI3)
```

`esp32-hal-spi.c` sizes its internal bus table with `SPI_COUNT`, confirmed
against `esp32c5/include/soc/soc_caps.h`'s `SOC_SPI_PERIPH_NUM=2` (flash-
shared SPI0/SPI1 plus one GP SPI2 — no SPI3) to be **1** on this chip.
`spiStartBus()` bounds-checks the requested bus index against `SPI_COUNT` and
returns `NULL` (no exception, no compile error) if it's out of range,
logging only at a level `platformio.ini`'s `-DCORE_DEBUG_LEVEL=0` build flag
suppresses by default. **`SPIClass(HSPI)` on a C5 (or C2/C3/C6/H2) passes an
out-of-range bus index and silently no-ops** — indistinguishable from working
code unless `CORE_DEBUG_LEVEL` is temporarily raised.

**The no-arg default constructor is not a safe fallback either** —
`libraries/SPI/src/SPI.h`: `SPIClass(uint8_t spi_bus = HSPI);`. Since `HSPI`
is `1` on this chip family (not `0`), a bare `SPIClass mySPI;` is exactly as
broken as `SPIClass mySPI(HSPI);`. The framework's own global `SPI` singleton
avoids this — `libraries/SPI/src/SPI.cpp` explicitly instantiates
`SPIClass SPI(FSPI);` for this chip family (a separate `SPIClass
SPI(VSPI);` line handles classic ESP32) rather than relying on the default
parameter, which is why `Storage::init()`'s `SPI.begin(...)` (shared
LCD/SD bus) has always worked fine — it was never exposed to this trap.
**Always pass `FSPI` explicitly when constructing a second `SPIClass`
instance on this chip family; never rely on the default constructor.** This
is also why an earlier LED debugging attempt that used `SPIClass(HSPI)`
looked like "SPI never sent anything" — see `open-questions.md` #1.

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
