# APA102 status LED

Single APA102 (or SK9822-compatible clone) RGB LED, driven in
`firmware/src/hal/hal.cpp` (`sendAPA102()`) — no library, per `AGENTS.md`'s
"earn your abstractions" rule (a one-LED driver doesn't need FastLED).

> **Pins — RESOLVED on hardware 2026-08-30 (`open-questions.md` #1).**
> - **T-Dongle-C5:** the APA102 is on **GPIO2 (data) / GPIO6 (clock)** — the
>   SAME pins as the LCD/SD SPI bus (MOSI=2, SCK=6). The vendor's
>   `pin_config.h` claim of `LED_DI=5`/`LED_CI=4` (the JTAG pads) is **wrong**;
>   driving 4/5 never reaches the LED. Because the LED shares the SPI bus, it
>   is driven over the shared hardware `SPI` (`CFG_LED_SHARED_SPI`), not
>   bit-banged, and re-latched after any LCD/SD traffic (`Hal::ledRefresh()`)
>   since the CS-less APA102 also sees LCD/SD data on the bus. Sources: on-
>   hardware A/B pin test (`led_pinmap_diag.cpp`) + `zombodotcom/T-Dongle-C5`.
> - **T-Dongle-S3:** dedicated pins GPIO40 (data) / GPIO39 (clock), bit-banged
>   (`CFG_LED_SHARED_SPI=0`). Not on the LCD bus. Unverified (no S3 hardware).

## Protocol basics

Frame layout, `VERIFIED (source)` against the original protocol write-ups
([Tim's blog, "APA102 aka Superled"](https://cpldcpu.com/2014/08/27/apa102/);
[Hackaday's protocol dig](https://hackaday.com/2014/12/09/digging-into-the-apa102-serial-led-protocol/)):

1. **Start frame:** 32 zero bits.
2. **LED frame** (one per LED, MSB first): `111bbbbb gggggggg rrrrrrrr bbbbbbbb`
   — top 3 bits fixed `111`, next 5 bits are a **global brightness register**
   (0–31), then full 8-bit G, R, B channels. So the "brightness" is a *second*,
   independent dimming control on top of the 0–255 color values — total dynamic
   range is `color × brightness`, up to `255 × 31`.
3. **End frame:** needs enough clock pulses to shift the last LED frame all the
   way through a chain. **The APA102 datasheet says to use `0xFFFFFFFF` here,
   and that's wrong** — `0xFFFFFFFF` is indistinguishable from a valid "max
   brightness white" LED frame, so on some parts/clones it can visibly
   corrupt the last LED instead of terminating cleanly
   ([cpldcpu, "SK9822 – a clone of the APA102?"](https://cpldcpu.com/2016/12/13/sk9822-a-clone-of-the-apa102/)).
   A stream of **zero bits** (32, or more for longer chains) works as an end
   frame on genuine APA102s and is *required* on SK9822 clones (they only latch
   new PWM values on the *next* start-frame-preceded write, so a zero "reset
   frame" both terminates the current write and pre-arms the next one). Zeros
   are therefore the portable choice regardless of which chip is actually on
   the board — this repo's Pololu-library-style zero end frame
   (`firmware/src/hal/hal.cpp`) matches that guidance.

## This board's specific recipe

Vendor ground truth, `Xinyuan-LilyGO/T-Dongle-C5` `examples/LED/led.ino` and
`examples/Factory/Factory.ino` — both use the Pololu `APA102` Arduino library
(vendored in the same repo at `lib/apa102-arduino`) and both call:

```c
ledStrip.sendColor(red, green, blue, 10);   // brightness register = 10, not 31
```

That confirms two things independently of this repo's own hardware debugging:

- **Brightness 10 (not 31) is the vendor's own choice**, not a workaround this
  project invented. `firmware/src/hal/hal.cpp`'s `HAL_APA102_BRIGHTNESS = 10`
  ("D2 recipe") matches the factory default. `VERIFIED (source)`.
- The vendor's LCD backlight init (`Factory.ino`: `digitalWrite(PIN_LCD_BL, 0)`)
  is **active-low** — same polarity this repo's `platformio.ini`
  (`TFT_BACKLIGHT_ON=LOW`) and `display.cpp` now assume. `VERIFIED (source)`.

Note on backlight: an earlier version of this repo forced the LCD backlight
**fully off** (`CFG_LCD_BL_LEVEL 0`) on the theory that its bleed washed the
LED "white" through the housing. That theory was **wrong** — the "white" was
the pin bug (the LED was on GPIO2/6 and never driven; see above and
`open-questions.md` #1), not backlight bleed. The vendor runs LED and backlight
together because there is no conflict. Backlight is restored
(`CFG_LCD_BL_LEVEL 180`); LED and dashboard both verified working
simultaneously on hardware 2026-08-30.

## Brightness field encoding, for anyone hand-rolling the frame

```c
uint32_t frame = (0xE0 | brightness) << 24 | (b << 16) | (g << 8) | r;
```

`0xE0` is `111 00000` — the fixed top-3-bit header with brightness bits
zeroed, OR'd with the 5-bit brightness value (`0–31`, values above 31 will
corrupt the header bits — mask/clamp if brightness is ever computed rather than
a literal).
