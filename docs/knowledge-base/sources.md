# Sources

Full bibliography for claims made across this knowledge base. Grouped by topic;
each entry has one line on what it's cited for. Refetch rather than trust a
paraphrase — several entries in `open-questions.md` exist precisely because a
paraphrase (including an AI-generated one, in this same research pass) missed
something a full read caught.

## ESP32-C5 silicon / USB

- [ESP32-C5 Datasheet — Espressif Documentation](https://documentation.espressif.com/esp32-c5_datasheet_en.html) — strapping pins, JTAG signal-to-GPIO table, §4.2.1.5 USB Serial/JTAG-only claim.
- [espressif/esp-idf#18625](https://github.com/espressif/esp-idf/issues/18625) — "ESP32-C5: USB-OTG (TinyUSB device mode) target support missing." `Resolution: Won't Do`. The correct citation for "no OTG device-mode software support."
- [espressif/esp-usb#371](https://github.com/espressif/esp-usb/issues/371) — "ESP32C5 support on Esp-IDF 6.0-beta1" (TinyUSB MSC build error). `Resolution: Done`. Do not cite this as supporting "Won't Do" — see `open-questions.md` #3.
- [ESP-IDF Programming Guide — Configure Other JTAG Interfaces (ESP32-C5)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/jtag-debugging/configure-other-jtag.html) — GPIO2–5 JTAG pin mapping, `usb_jtag_bridge_en` behavior description.
- [`esp-idf` source: `components/soc/esp32c5/register/soc/usb_serial_jtag_struct.h`](https://raw.githubusercontent.com/espressif/esp-idf/master/components/soc/esp32c5/register/soc/usb_serial_jtag_struct.h) — exact `conf0` register bitfield definitions and reset values; primary source for `open-questions.md` #1.
- [ESP32 Strapping Pins List — espboards.dev](https://www.espboards.dev/blog/esp32-strapping-pins/) — strapping pin roundup across ESP32 variants.

## LilyGO T-Dongle-C5 (vendor)

- [`Xinyuan-LilyGO/T-Dongle-C5`](https://github.com/Xinyuan-LilyGO/T-Dongle-C5) — official factory firmware repo. Ground truth for pin map, LED brightness/backlight recipe.
  - `include/pin_config.h`, `examples/Factory/pin_config.h` — pin definitions, matches this repo's `config.h` exactly.
  - `examples/Factory/Factory.ino` — backlight active-low init (`digitalWrite(PIN_LCD_BL, 0)`), LED brightness=10 via Pololu APA102 lib.
  - `examples/LED/led.ino` — standalone LED demo, same brightness=10 recipe, plain `pinMode`/Pololu lib with no JTAG register writes.
  - `lib/apa102-arduino/` — vendored Pololu APA102 Arduino library (upstream: [pololu/apa102-arduino](https://github.com/pololu/apa102-arduino)).
- [LilyGO T-Dongle-C5 Quick Start wiki](https://wiki.lilygo.cc/products/t-dongle-series/t-dongle-c5/quick-start.html) — secondary reference; prefer the firmware repo's `pin_config.h` if they ever disagree.

## APA102 / SK9822 protocol

- [cpldcpu, "APA102 aka Superled" (2014)](https://cpldcpu.com/2014/08/27/apa102/) — original protocol reverse-engineering.
- [cpldcpu, "SK9822 – a clone of the APA102?" (2016)](https://cpldcpu.com/2016/12/13/sk9822-a-clone-of-the-apa102/) — end-frame `0xFFFFFFFF` datasheet error, SK9822 reset-frame requirement, why zero end frames are the portable choice.
- [Hackaday, "Digging Into The APA102 Serial LED Protocol" (2014)](https://hackaday.com/2014/12/09/digging-into-the-apa102-serial-led-protocol/) — independent protocol write-up.
- [Bus Pirate docs — APA102/SK9822](https://docs.buspirate.com/docs/devices/apa102-sk9822/) — quick reference.

## TFT_eSPI / ST7735

- [`Bodmer/TFT_eSPI#3751` — "Support For ESP32-C5 ECO2"](https://github.com/Bodmer/TFT_eSPI/issues/3751) — the C3→C5 processor-file copy workaround and the `SPI.begin()`-before-`tft.init()` fix that actually got a picture on screen (see `open-questions.md` #2 for why you need the full comment thread, not just the issue body).
- [`bodmer/TFT_eSPI`](https://github.com/Bodmer/TFT_eSPI) — upstream library repo.

## Toolchain

- [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) — the PlatformIO platform fork used for all builds in this repo.
- [CNX Software — "platform-espressif32 fork to enable PlatformIO support for ESP32-C6, ESP32-C5, ESP32-H2, and ESP32-P4"](https://www.cnx-software.com/2024/08/27/platform-espressif32-fork-platformio-arduino-esp32-c6-esp32-c5-esp32-h2-esp32-p4/) — why the fork exists; official platform-espressif32 has no C5 support.
- [espressif/arduino-esp32#8238](https://github.com/espressif/arduino-esp32/issues/8238) — "ESP32-S3 does not boot until Serial console is attached (Using CDC on Boot)" — the general USB-CDC boot-race class of issue this project's `CFG_SERIAL_CONNECT_WAIT_MS` addresses.

## This repo's local hardware bring-up dossier (untracked — see `open-questions.md` #6)

- `.hermes/plans/2026-08-29_c5-hardware-dossier.md` — the single best source in
  this project for T-Dongle-C5 hardware status: a chronological evidence table
  of every LED experiment (settings + observed result), the verified-working
  subsystem list, and the 2026-08-30 LED regression. **Gitignored — local to
  this machine/clone only, not in git history.** Refetch by reading the file
  directly if you have filesystem access to this checkout; otherwise it's gone.
- `.hermes/plans/2026-08-28_093918-tdongle-c5-bringup.md` — the task-by-task
  bring-up plan and phase-by-phase execution log this dossier summarizes.
  Same untracked caveat applies.

## Live hardware session, 2026-08-30 (this session — see `open-questions.md` #1)

With direct access to the attached T-Dongle-C5 (`/dev/ttyACM0`, `303a:1001`),
resolved the LED question by testing rather than reading further sources:
- Reflashed LilyGO factory firmware (`/tmp/factory.bin`) — user-observed dim
  purple/pink blend with white flashing.
- Flashed a from-scratch static-frame diagnostic (`/tmp/hwdiag`, not part of
  this repo) sending one held colour (RED/GREEN/BLUE/OFF/AMBER, 3s each, one
  frame per colour, nothing else running) — user-observed no change at all
  across two runs, same blended appearance.
- Repeated the same diagnostic a third time across a genuine physical
  unplug/replug (user-confirmed, not just a VM/passthrough reconnect) to rule
  out a recoverable stuck-latch state in the APA102 itself (soft resets alone
  never drop USB VBUS, so the LED chip's own internal state was never actually
  power-cycled before this). Result: still frozen — constant white, no colour
  changes through the same sequence. Rules out "it's a recoverable state, not
  permanent damage."
- Restored the project's own firmware afterward; device confirmed booting
  clean (SD, WiFi C2, SoftAP) post-restore, each time it was reflashed.
- Conclusion: confirmed hardware fault (dead/stuck LED or connection), not a
  firmware bug. Neither `/tmp/factory.bin` nor `/tmp/hwdiag` are part of this
  repo — they're session-local artifacts; the diagnostic source is reproducible
  from the description in `open-questions.md` #1 if needed again.

## This repo (primary source for toolchain-drift and vendor-parity claims)

- Commits `95f06c7`, `40d37b8`, `a56b3d8`, `b284e1b` — the LED/backlight bring-up sequence on `fix/c5-console-boot-race`.
- Commits `c489531`, `fb6ec7b` — Arduino-ESP32 3.x API drift fixes (USB HID API, namespace/forward-declare, `File::name()` return type, UTF-8 literal removal).
- Commit `cf2f446` — TFT_eSPI vendoring for C5 support.
- `AGENTS.md`, `CONTRIBUTING.md`, `docs/programming-style.md` — this repo's own architecture/convention docs; treat as authoritative for *this codebase's* decisions, cross-check against the external sources above only for hardware/vendor-library facts.
