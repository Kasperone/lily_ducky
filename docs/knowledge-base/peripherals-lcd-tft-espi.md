# LCD — ST7735 via TFT_eSPI

0.96" 80×160 IPS ST7735 panel over SPI, driven with `bodmer/TFT_eSPI`. Both boards
use the same panel and library; only pins differ (`firmware/platformio.ini`).

## Upstream TFT_eSPI has no ESP32-C5 support — this repo vendors a patched copy

`VERIFIED (source)`. `bodmer/TFT_eSPI` (as of 2.5.43 and current `master`) ships
processor files for ESP32/S2/S3/C3 but none for C5 —
[`Bodmer/TFT_eSPI#3751`](https://github.com/Bodmer/TFT_eSPI/issues/3751)
("Support For ESP32-C5 ECO2") is the tracking issue, closed `completed` without a
merged upstream fix. `firmware/lib/TFT_eSPI/` is vendored in-repo (not a
`lib_deps` entry) specifically so the patch survives; see commit `cf2f446` and the
comment in `platformio.ini`.

The fix (`Processors/TFT_eSPI_ESP32_C5.c/.h`) is copied from the C3 processor
files with names/defines swapped — valid because C3 and C5 share the same
`GPSPI2` SPI peripheral and IDF5 register layout, per the same commit's message.
This mirrors exactly what the issue reporter (`justcallmekoko`) describes doing
themselves: *"I fixed this issue by simply copying the processor files for the
ESP32-C3 and just renaming any filenames and definitions to reflect C5 instead of
C3."*

### The part that's easy to miss: compiling isn't the same as working

Reading only the issue title/first post, it looks fully resolved. **It wasn't, at
first** — the original reporter's own post says the C3→C5 copy fixed *compilation*
but the display stayed **blank**: *"I have not been able to actually get the
display to display anything... my knowledge of SPI is not where it needs to be."*
The actual fix that made pixels appear came from a different commenter,
`HonestQiao`, two months later, in the full issue thread (fetch via
`api.github.com/repos/Bodmer/TFT_eSPI/issues/3751/comments`, not just the issue
page — GitHub's issue page doesn't render comments in a plain fetch):

```c
SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);   // must run before tft.init()
tft.init();
```

The C5 processor port apparently doesn't configure the SPI bus pins itself the way
other processor files do — you must call the Arduino `SPI.begin()` with explicit
pins yourself, first. `firmware/platformio.ini`'s comment claiming this vendoring
is "the fix validated on real C5 hardware in Bodmer/TFT_eSPI#3751" is *true of the
combination* (processor-file copy + explicit `SPI.begin()`) but glosses over the
fact that the processor-file copy *alone*, per the issue's own original poster,
produced a blank screen. If you only read that platformio.ini comment or the
commit message, you'd reasonably assume vendoring the processor file was
sufficient — it wasn't, for the person who validated it.

### This repo already does the required `SPI.begin()` — but not because it knew why

Checked directly against the boot order in `firmware/src/main.cpp`:

```
Hal::init()  →  Storage::init()  →  Display::init()  →  lcdInit()  →  _tft.init()
```

`Storage::init()` (`firmware/src/storage/storage.cpp:52`) calls
`SPI.begin(PIN_LCD_SCK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_SD_CS)` **unconditionally**
(before checking whether an SD card is even present), and it runs before
`Display::init()`. So the exact sequencing `HonestQiao` needed is already
satisfied — the SPI bus is `.begin()`'d, with explicit pins, before `tft.init()`
runs. `VERIFIED (source; code inspection)` — not yet re-confirmed against a lit
screen on this repo's own hardware in this session. If the LCD is ever reported
blank on this board, do not re-derive this fix from scratch — check first whether
someone reordered `Storage::init()` after `Display::init()`, which would silently
reintroduce exactly this bug.

## Panel geometry and polarity — confirmed settings

- **80×160 visible inside 132×162 controller RAM**: needs the `ST7735_GREENTAB160x80`
  variant define and its baked-in x/y start offsets — already set in
  `platformio.ini`. Don't hand-roll offsets; picking the wrong "tab" variant is
  the most common ST7735 symptom (image shifted / wrapped a few pixels).
- **`TFT_RGB_ORDER=TFT_BGR`, `TFT_INVERSION_ON=1`**: required on this panel
  (`AGENTS.md`), consistent with common ST7735 IPS-variant behavior — the
  inversion bit and color order are panel-batch properties, not driver bugs, so
  don't "fix" swapped colors by remapping channels in application code instead.
- **Backlight active-LOW**: confirmed against vendor `Factory.ino`
  (`digitalWrite(PIN_LCD_BL, 0)` = on) — see `peripherals-apa102-led.md`. Applies
  to the C5 only; the S3's backlight (`TFT_BL=37`) is active-HIGH per
  `platformio.ini` and is unverified on real S3 hardware (board not yet acquired,
  per `AGENTS.md`).
- **Rotation**: current code uses `setRotation(3)`, changed from `setRotation(1)`,
  commented as "LilyGO factory orientation." Vendor `Factory.ino` (using
  `Adafruit_ST7735`, a different library) calls `tft.setRotation(3)` too — same
  numeric value, supporting evidence only, since rotation enum meaning isn't
  guaranteed identical across driver libraries. Treat as `PLAUSIBLE (unverified)`
  until confirmed against this repo's own hardware with TFT_eSPI specifically.
