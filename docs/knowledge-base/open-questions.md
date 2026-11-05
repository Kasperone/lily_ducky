# Open questions — read this before trusting a hardware claim in this repo

Things found while building this knowledge base (2026-08-30) where an existing
commit message, code comment, or `AGENTS.md`/`README.md` line either didn't hold
up against a primary source, or couldn't be told apart from an unverified guess.
#3 and #5 were fixed directly (docs-only, low risk). #4 was fixed by rewriting
the status banners once real hardware evidence was found. **#1 has been
through two "confirmed dead" → retracted cycles already** — read the full arc
before trusting any summary of it, including this one; a same-day 2026-08-30
addendum (a *second* research-and-test session, after the first arc below)
used a genuinely hardware-timed driver, eliminated every confound the first
arc found, and produced the strongest evidence yet for a hardware fault —
still short of calling it settled, for reasons the addendum states explicitly.
#6 remains open as a process question for the repo owner. Each entry has the
evidence so whoever (Hermes, Claude, or the repo owner) picks this up next
doesn't have to redo the research.

## 1. LED — RESOLVED 2026-08-30 (third pass): it was a PIN BUG, not dead hardware. The APA102 is on GPIO2/6 (the LCD SPI bus), NOT GPIO4/5. Fixed and verified on hardware.

> **RESOLUTION (2026-08-30, live hardware, `/dev/ttyACM0`, board MAC
> `38:44:BE:BC:F9:3C`).** The status LED works. It was never dead. The
> APA102's data/clock are wired to **GPIO2 (data/MOSI) and GPIO6 (clock/SCK)
> — the same pins as the LCD/SD SPI bus** — not GPIO4/5 (the chip's JTAG
> MTCK/MTDO pads) as the vendor's own `pin_config.h` (`LED_DI=5`, `LED_CI=4`)
> claims and as this whole investigation assumed. Every prior test drove 4/5,
> which never reaches the LED; the "solid white/amber/cyan" everyone saw was
> the LED holding **stale residual data left on the shared bus**, which is
> also exactly why it read as "intermittent" (different leftover colours) and
> why the vendor factory firmware showed "dim purple/pink with white
> flashing" (the LED displaying live LCD SPI traffic as garbage).
>
> **How it was proven:** `firmware/src/diag/led_pinmap_diag.cpp` (envs
> `T-Dongle-C5-pinmap-A` = GPIO2/6, `-pinmap-B` = GPIO5/4) drives an APA102
> colour cycle on each candidate pin pair, LED-only. On hardware, user
> watching: **Phase A (2/6) cycles red/green/blue/off/white correctly;
> Phase B (5/4) stays frozen** on a static colour. Then the main firmware,
> fixed to drive 2/6 over the shared SPI bus, showed a steady teal status LED
> **and** the LCD dashboard simultaneously — both verified live.
>
> **The fix (committed, verified on hardware):**
> - `config.h`: `PIN_LED_DATA=2`, `PIN_LED_CLOCK=6` (was 5/4); new
>   `CFG_LED_SHARED_SPI` flag (1 on C5, 0 on S3).
> - `hal.cpp`: `sendAPA102()` drives the LED over the global (shared) Arduino
>   `SPI` on the C5 instead of bit-banging JTAG pins; new `Hal::ledRefresh()`
>   re-latches the colour after LCD/SD bus traffic (the APA102 has no CS, so a
>   paint clocks garbage through it).
> - `display.cpp`: `lcdUpdate()` reports whether it painted; `Display::update()`
>   calls `Hal::ledRefresh()` after a paint.
> - `config.h`: LCD backlight restored (`CFG_LCD_BL_LEVEL` 0 → 180) — the
>   "backlight washes the LED white" theory was this same pin bug.
> - `usb_jtag_bridge_en`/`CFG_RELEASE_JTAG_LED_PINS`: now **moot** for the LED
>   (it isn't on the JTAG pins at all). Left as a dead 0 macro. The register's
>   documented direction (below) is still correct; it just never mattered here.
>
> **Primary source that cracked it:** `github.com/zombodotcom/T-Dongle-C5`, a
> community examples repo, documents the 2/6 shared-bus wiring in raw code
> (`Adafruit_DotStar rgbLed(1, 2, 6, ...)`) and a detailed README (the
> `0x0001`-not-`0x0000`-background flicker quirk is the tell of real hardware
> debugging). This **conflicts with the vendor `pin_config.h`** — the vendor
> is wrong for the LED on this board. Lesson consistent with this file's
> theme: the vendor is usually ground truth, but "usually" isn't "always,"
> and a cheap A/B hardware test beats trusting either source when they
> conflict. See `peripherals-apa102-led.md`, `hardware-esp32-c5.md`, and
> `sources.md`.
>
> **Board-identity gap (previously open) also closed:** the attached unit's
> factory MAC (USB iSerial) is `38:44:BE:BC:F9:3C`, a *different* physical
> board than the `38:44:BE:BC:FA:C4` in the dossier §1 (same batch). So the
> unit fixed here is not the one the earliest arc sections describe — but the
> pin bug is a wiring fact of the board design, not a per-unit condition, so
> it applies to all of them.
>
> Everything below this box is the pre-resolution historical arc, kept as the
> record of how two "confirmed dead" conclusions were reached and retracted
> before the actual (pin) cause was found. **It is superseded — do not act on
> its "next steps."**

**Where:** `firmware/src/hal/hal.cpp` (`Hal::init()`, `sendAPA102()`),
`firmware/src/config.h` (`CFG_RELEASE_JTAG_LED_PINS`, `HAL_APA102_BRIGHTNESS`).

**Do not skip to a conclusion here — this entry was wrong once already, twice
in one session, and both times because a genuinely clean-looking result was
trusted before a cheap follow-up test would have caught the problem.** Recording
the full arc, including the dead ends, because the two mistakes are the more
valuable lesson than any single-board finding.

### Board 1 (the original unit) — tests and the premature conclusion

1. Reflashed **LilyGO's stock factory firmware** (`/tmp/factory.bin`, Arduino
   framework, Pololu APA102 library — itself `digitalWrite`-based bit-banging,
   not a hardware-timed driver). User-observed: dim purple/pink blend with
   occasional white flashing — never a clean, distinct colour.
2. Flashed a **from-scratch diagnostic** (`/tmp/hwdiag`, our own
   `digitalWrite`-bit-banged recipe, `bridge_en=1`) sending one static frame
   per colour (RED/GREEN/BLUE/OFF/AMBER, 3s each, nothing else running).
   User-observed, twice: no change at all, same purple/pink-white blend. A
   gentle press near the LED/PCB produced no change (rules out a
   flex-sensitive cracked joint specifically).
3. Repeated the same diagnostic across a genuine **physical unplug/replug**
   (real power cycle, user-confirmed) to rule out a recoverable stuck-latch
   state in the APA102 itself. Still frozen: constant white.
4. **At this point the session concluded "confirmed hardware fault, not a
   firmware bug" and closed this entry out.** That conclusion did not survive
   contact with a second unit (below) — every test above used
   `digitalWrite`-bit-banged drivers (ours and LilyGO's own Arduino example),
   and board 2 showed that *this specific class of driver* can fail even on
   confirmed-healthy hardware. Board 1's LED may or may not actually be
   damaged — it was never tested with a hardware-timed driver (RMT/proper SPI)
   the way board 2 eventually was, and by the time that gap was noticed, board
   1 had already been swapped out. **This is unfinished, not disproven.**

### Board 2 (a second, physically different unit — different MAC address, confirmed by boot log) — where it got more complicated, not less

5. Booted whatever shipped on it: a **native ESP-IDF** example ("blink
   addressable LED", not Arduino, distinct from the Pololu-library factory
   image on board 1) — user-observed genuine "various colours changing"
   correctly. This is evidence the vendor's own hardware-timed/RMT-style LED
   driver works fine on healthy hardware, distinct from bit-banged approaches.
6. Flashed our own diagnostic (`bridge_en=1`, bit-banged, RED/GREEN/BLUE/
   OFF/AMBER, 3s each). User-observed: **stuck on amber the entire time** —
   different frozen symptom than board 1's white, but still frozen.
7. Same diagnostic with `bridge_en` left at default (0), matching what the
   working vendor example implicitly relies on. User-observed: **nothing at
   all** (no light) — a third distinct frozen symptom.
8. Hypothesis at this point: maybe `bridge_en` itself is the active problem —
   its documented behavior (routing the *internal* JTAG bridge's own signal
   onto MTMS/MTDI/MTCK/MTDO via the GPIO matrix) could mean it overrides
   whatever we try to drive on those pins with something else entirely,
   independent of how we generate the signal. Tested this by switching from
   `digitalWrite` bit-banging to **hardware SPI** (`SPIClass(HSPI)`,
   `SPI.begin(sck=4, miso=-1, mosi=5, ss=-1)`) — a completely different signal
   path with a fixed hardware clock, no CPU-loop jitter. Result with
   `bridge_en=1`: **still stuck on amber, identical to the bit-banged result.**
   This looked like strong confirmation that `bridge_en` overrides the pin
   output regardless of source.
9. **Second mistake, caught in time:** before trusting step 8, ran a direct
   verification — set the LED to a known, distinctive colour (bright cyan)
   via the *already-trusted* bit-bang method, held it, then immediately tried
   to override it via the new SPI code with a different colour. **The LED
   stayed cyan the whole time.** This proves the SPI code never actually
   transmitted anything — `SPIClass(HSPI)` with `miso=-1` most likely never
   initialized correctly on the ESP32-C5's single GPSPI2 peripheral (the
   `HSPI`/`VSPI` bus naming is inherited from classic dual-SPI-controller
   ESP32s and may not map cleanly here — not confirmed, just the leading
   guess). **Every SPI-based result in step 8 was therefore an artifact —
   stale cyan/amber carried over from whatever the previous bit-banged test
   had last successfully written, not a real reading.** Discard step 8's
   "SPI confirms bridge_en overrides everything" claim entirely.
10. With the SPI confound identified, retested the **plain bit-bang cycle
    again** (RED/GREEN/BLUE/OFF/AMBER, `bridge_en=1`, longer 4s holds, this
    time properly synced with a "say go when watching" protocol to rule out
    the observer missing early transitions). Result: **stuck again** — not
    cycling. But step 5's cyan write, using the *identical* bit-bang code path
    and `bridge_en=1`, had worked moments earlier in the SPI-verification test.

### Where this actually leaves things

The same exact bit-banged code, same board, same `bridge_en` setting,
produced a correct result once (cyan, step 9's baseline) and a stuck result
immediately after (step 10) — genuine **intermittency**, not a clean
deterministic bug. That rules out a simple "this register direction is always
wrong" or "this specific LED die is always dead" story — both would be
reproducible every time, not hit-or-miss. Intermittency instead points toward
something marginal: a signal-integrity issue (borderline timing, a weak
connection that sometimes makes contact) that is sensitive to conditions not
yet identified (temperature, exact CPU load/interrupt timing during the
bit-bang loop, which specific pin transition sequence preceded it, etc.).

**What we can say with actual confidence:**
- Two boards, two different Arduino-framework Pololu/bit-bang firmwares, both
  showed frozen/wrong LED output at least some of the time.
- One board's non-Arduino, hardware-timed vendor example worked correctly
  every time it was watched.
- `bridge_en`'s effect could not be cleanly isolated — the one test that
  seemed to isolate it (step 8) was invalidated by a tooling bug (step 9), and
  a clean bit-bang-only re-test (step 10) then contradicted step 5's own
  bit-bang result on the same board/setting.

**What we cannot say:** that either board's LED is definitively dead, that
`bridge_en`'s direction is settled, or that the intermittency has a known
cause. The original "confirmed hardware fault" framing for board 1 is
retracted — it was reached honestly, from real tests, but the tests all shared
an untested assumption (that bit-banged `digitalWrite` output is a reliable
enough signal source to draw conclusions from) that board 2 then falsified.

**Recommended next steps, in order of cost — #1 done 2026-08-30, see the dated
addendum below; #1's own "RMT-backed led_strip" framing was itself wrong and
is corrected there too, read it before acting on this list:**
1. ~~Write and test a genuinely hardware-timed driver~~ — done 2026-08-30,
   see addendum. **Correction:** ESP-IDF's `led_strip` component does NOT use
   RMT for APA102/SK9822 — its own docs state those chips use the component's
   *SPI* backend; RMT is for single-wire timing-critical protocols
   (WS2812/NeoPixel-style), which the two-wire clock+data APA102 is not.
   "Hardware-timed" for this LED means hardware SPI, not RMT — tested
   directly (not via the component, which would drag in an ESP-IDF managed
   component into an Arduino/pioarduino build for one LED) in
   `firmware/src/diag/led_spi_diag.cpp`.
2. If a hardware-timed driver is also intermittent, suspect a genuine signal
   integrity issue (wire length/quality between MCU and LED, pull resistor
   values, decoupling) rather than firmware — that needs a logic analyzer or
   oscilloscope, out of scope for a coding session. **Update:** the
   2026-08-30 hardware-SPI test was NOT intermittent — clean, deterministic,
   reproducible non-response every time, including under a raw all-zero
   stream a healthy LED cannot stay lit through. That's a different (stronger)
   result than this step anticipated; see the addendum for what it implies.
3. Only once a driver is shown reliable across *many* repeated on/off/color
   cycles (not one lucky run) should any board's LED be declared working or
   dead, and only then does the `bridge_en` direction question become
   testable again. **Update:** the direction question is no longer
   theoretical — the full register text (not just the excerpt originally
   quoted) states plainly that `bridge_en=1` forces MTDO (GPIO5, this board's
   LED **data** pin) into INPUT direction. Retested with the bit left at its
   default (0, matching the vendor) 2026-08-30 — result unchanged. See
   addendum.
4. **Process lesson for next time, independent of the LED itself:** when a
   test result seems to cleanly confirm a hypothesis, spend one extra cheap
   test trying to break the result before writing it down as fact (step 9
   above is the template) — especially when the test itself introduces new,
   untested code (a new driver, a new library call) rather than only changing
   the one variable under investigation. **Followed 2026-08-30**: the
   hardware-SPI result was checked against a silent-init-failure explanation
   (verbose core logging — the exact class of failure that explained the
   earlier SPIClass(HSPI) attempt) before being trusted; see addendum.

### Addendum, 2026-08-30 — hardware SPI tested, both `bridge_en` states tested, still non-responsive; this is the strongest evidence yet, still not being called "confirmed dead"

Session with direct hardware access (`/dev/ttyACM0`, `303a:1001` attached —
board identity not cross-checked against boot-log MAC against the board(s)
tested in the prior arc above; treat this as "whichever unit is currently
plugged in," not confirmed as board 1 or board 2).

**What was fixed/corrected before testing, each independently sourced:**

- **`SPIClass(HSPI)`'s silent failure (step 8/9 above) is now root-caused, not
  just discarded as a confound.** `cores/esp32/esp32-hal-spi.h` in
  arduino-esp32 defines, for the C2/C3/C5/C6/C61/H2 family (one
  general-purpose SPI peripheral each): `FSPI=0`, `HSPI=1` — with `HSPI`'s
  comment explicitly attributing it to "ESP32S2, S3, P4 - SPI 3 bus," chips
  that have a *second* GP-SPI peripheral, which the C5 does not (confirmed
  against `esp32c5/include/soc/soc_caps.h`: `SOC_SPI_PERIPH_NUM=2`, counting
  the flash-shared SPI0/SPI1 plus one GP SPI2 — no SPI3). `esp32-hal-spi.c`
  sizes its bus table with `SPI_COUNT`, which is **1** for this chip family;
  `spiStartBus()` bounds-checks `spi_num >= SPI_COUNT` and returns `NULL` on
  failure, logging at a level `platformio.ini`'s `CORE_DEBUG_LEVEL=0` build
  flag suppresses by default. `SPIClass(HSPI)` on a C5 passes `spi_num=1`,
  which is out of range — an invalid-argument bug, not a deeper signal
  problem, and it explains why that step's result was indistinguishable from
  "did nothing." **`FSPI` is the only valid bus on this chip.**
- **The "RMT-backed led_strip" framing (this file's own step 1, `AGENTS.md`,
  `config.h`) was wrong.** Espressif's `led_strip` component docs state
  APA102/SK9822 use the component's SPI backend specifically; RMT backs
  single-wire protocols. Corrected everywhere it was stated as fact.
- **The `usb_jtag_bridge_en` register's full text** (previously only an
  excerpt was quoted; refetched in full from
  `usb_serial_jtag_struct.h`'s `conf0` union) confirms, verbatim: *"Set this
  bit usb_jtag, the connection between usb_jtag and internal JTAG is
  disconnected, and MTMS, MTDI, MTCK output via GPIO Matrix, MTDO inputs via
  GPIO Matrix."* GPIO5 (MTDO) is `PIN_LED_DATA`. Setting this bit forces that
  specific pin into **input** direction at the peripheral level — no amount
  of `pinMode`/`digitalWrite`/`SPIClass` code on our side can drive it as
  output while the bit is set. GPIO4 (MTCK/`PIN_LED_CLOCK`) stays an output
  either way. That is a clean mechanism for "clock toggles, data can't" —
  which reads on an APA102 as a fixed, data-independent output.
- **The ESP32-C5's own "Configure Other JTAG Interfaces" guide** states JTAG
  is connected to the built-in USB_SERIAL_JTAG peripheral **by default**, and
  is only bridged onto physical GPIO2–5 when explicitly asked for (this
  register bit, or burning the `DIS_USB_JTAG`/`JTAG_SEL_ENABLE` eFuses for an
  external probe) — i.e. GPIO2–5 are not inherently "owned" by JTAG at reset
  the way `config.h`'s original comment assumed; the vendor firmware's total
  silence on this register isn't an oversight, it's correct, because there
  was never anything to release.

**Tests run, all on the currently-attached unit, all using real hardware SPI
(`SPIClass(FSPI)`, confirmed correctly bound to GPIO4=SCK/GPIO5=MOSI via
verbose `esp32-hal-periman` logging — `perimanSetPinBus(): Pin 4 ... SPI_MASTER_SCK`
/ `Pin 5 ... SPI_MASTER_MOSI`, no errors, ruling out a repeat of the
`SPIClass(HSPI)` silent-failure class of bug):**

1. `bridge_en=1` (matches main firmware's current `CFG_RELEASE_JTAG_LED_PINS`):
   proper APA102 frames (start frame, header+brightness+BGR, zero end frame),
   cycling RED/GREEN/BLUE/OFF/AMBER, 4s holds. **Result: solid, unchanging
   white**, independent of colour sent — including "OFF."
2. `bridge_en` left untouched (default 0, matching the vendor exactly — the
   register write skipped entirely, not just set to 0): identical test.
   **Result: unchanged — solid white.** This empirically closes the
   `bridge_en` direction question for practical purposes on this unit: even
   the "correct per the datasheet, matches the vendor" state doesn't produce
   working output, so whatever is wrong here isn't explained by that register
   alone.
3. Verbose (`CORE_DEBUG_LEVEL=5`) reflash of test 2 to positively rule out a
   silent SPI init failure of the same class as the `HSPI` bug: peripheral
   manager confirms clean, error-free binding of both pins to real SPI
   hardware. **Result: unchanged — solid white**, now with high confidence
   the MCU side is doing exactly what the code says.
4. Falsification test: raw, non-APA102-framed, **continuous all-zero-byte
   stream for 5 seconds** (1 MHz, well below anything that should stress
   GPIO-matrix-routed signal integrity), then continuous all-`0xFF` for 5s,
   then the normal colour cycle — all on the `bridge_en`-untouched,
   verbose-confirmed config from test 3. A functioning APA102/SK9822 **cannot
   stay lit through a sustained all-zero data stream** regardless of framing
   convention; this removes framing/protocol/end-frame-choice as a variable
   entirely. **Result: solid white throughout — including the 5s all-zero
   phase.** User-observed directly, watching live, confirmed via chat.

**What this rules out, with actual confidence:** bit-bang CPU-loop jitter (a
correctly-bound hardware SPI peripheral was used); the `SPIClass(HSPI)`
invalid-bus-index bug (root-caused and fixed, `FSPI` used and confirmed
bound); silent SPI driver init failure (verbose logging showed a clean
bind); `bridge_en` register direction as *the* explanation (tested with the
bit both set and left at the vendor-matching default — same result either
way); frame/protocol/end-frame choice (a raw all-zero stream bypasses all of
that and still didn't turn the LED off).

**What this does not, and cannot, rule out from a coding session:** whether
the ESP32's pad is *physically* toggling at the LED's own pins (no scope/logic
analyzer used — everything above is the SoC's own internal bookkeeping, not
an external measurement); a cold solder joint or trace damage between the
MCU and the LED package; damage to the LED die/driver itself; and — because
this session never cross-checked the boot-log MAC against the two units
described in the arc above — whether this is the same physical unit that
earlier showed varied, genuinely intermittent bit-banged symptoms (amber,
cyan, "nothing"), or the first unit that showed the *original* constant-white
result. The signature here (solid, unvarying white, unresponsive to every
tested driver/config/protocol combination) matches that first unit's
original finding far more closely than the second unit's intermittency.

**Where this leaves the "confirmed dead hardware" question, stated
carefully because that framing was retracted once already in this same
investigation:** this session's evidence is categorically stronger than
either of the two prior "confirmed"/"retracted" passes, because it is the
first to eliminate every confound identified in the earlier arc at once
(bit-bang jitter, the SPI init bug, the register-direction question, framing
choice) rather than trusting one clean-looking result. A hardware fault on
this specific unit — LED die, driver chip, or a bad connection between the
MCU and the LED — is now the best-supported explanation, better supported
than at any earlier point in this file. It is still not being written down
as flatly "confirmed," because the one thing that would make it so — an
external electrical measurement at the LED's own pins, or successfully
re-running the one driver that has ever worked in this whole investigation
(the vendor's native ESP-IDF example, step 5 in the arc above) on this exact
unit — was not done this session (no ESP-IDF/`idf.py` toolchain installed in
this environment; standing one up from scratch was judged not worth it given
how much software-side evidence already converges). Whoever picks this up
next with either of those tools available can close this out for real; until
then, treat "hardware fault, most likely permanent" as the leading,
well-evidenced hypothesis for the currently-attached unit, not as settled
fact.

**What this means for the register-direction question (item originally under
this heading):** **resolved as of the 2026-08-30 addendum above** — the full
register text is unambiguous (`bridge_en=1` forces MTDO/GPIO5/LED-data to
input), and it was empirically retested with the bit left untouched (matching
the vendor exactly): no change in outcome. `usb_jtag_bridge_en` should NOT be
set for this LED's use case — `CFG_RELEASE_JTAG_LED_PINS` in `config.h` is
based on a premise (JTAG owns these pins by default, needs releasing) that
the C5's own "Configure Other JTAG Interfaces" guide contradicts (JTAG is
only bridged onto GPIO2-5 when explicitly asked for). This no longer needs a
second unit to settle — it's settled by the primary source plus one clean
retest. What a second unit *would* still be useful for: confirming this
finding generalizes (i.e. that a healthy C5's LED genuinely works with
`bridge_en` left alone), since every test on the currently-attached unit has
been LED-non-responsive regardless of this setting.

**What this means for the project:** the status LED on the currently-attached
board does not respond to any correctly-verified driver or configuration this
session could produce — see the 2026-08-30 addendum for exactly what was and
wasn't ruled out. **`usb_jtag_bridge_en` should be left unset** regardless of
the LED's condition (it's wrong per the register's own documented behavior,
independent of whether it happens to matter for a given unit) —
`CFG_RELEASE_JTAG_LED_PINS` in `config.h` should be flipped to 0 the next time
someone is touching this code with hardware available to verify the change,
rather than left as a known-wrong default. Firmware LED logic (`Hal::ledSet`,
`ledBlink`, `sendAPA102`) is implemented correctly per protocol and per the
vendor's own recipe (brightness 10, zero end frame) — that part isn't the
problem. Options going forward, for whoever owns this hardware decision:
accept the LED as likely non-functional and rely on the LCD dashboard for
status (it already shows the same information); attempt a rework/reflow of
the APA102 package if equipment is available; bodge an external APA102/LED to
a spare GPIO; or, cheapest next step for a future session, an electrical
measurement (multimeter continuity from GPIO4/5 to the LED's pads; a logic
analyzer or scope on the data line during a raw-stream test like the one
above) or re-running the vendor's native ESP-IDF LED example on this exact
unit (needs an ESP-IDF toolchain, not installed in this environment) — either
would be genuinely conclusive in a way no further Arduino-side code change
can be.

## 2. "Validated on real C5 hardware" (TFT_eSPI vendoring) is true, but not for the reason stated

**Where:** `firmware/platformio.ini` comment, commit `cf2f446`.

**The claim:** vendoring the C3-derived `TFT_eSPI_ESP32_C5.c/.h` processor files
is "the fix validated on real C5 hardware in Bodmer/TFT_eSPI#3751."

**What's actually in that issue:** the *processor-file copy* (by the original
poster) did **not** get a picture on screen — they reported a blank display
despite verified wiring. What later got confirmed working (by a different
commenter, two months later) was that copy **plus** an explicit `SPI.begin()`
call before `tft.init()`. Reading only the issue title, or only the opening post,
misses this — you have to pull the full comment thread (`.../issues/3751/comments`
via the API, not the HTML issue page, which doesn't render dynamically-loaded
comments to a plain fetch).

**Resolution, not just a flag:** checked in this session — this repo's own boot
order (`Storage::init()` before `Display::init()` in `main.cpp`) already performs
the required `SPI.begin()` with explicit pins before `tft.init()` runs (see
`peripherals-lcd-tft-espi.md`). So the practical risk is low, *but* it's low by
accident of unrelated code (the SD card needing the same SPI bus), not because
anyone traced it to this requirement. If `Storage::init()` is ever reordered,
removed, or made conditional in a way that could skip on a card-less boot, this
gotcha reappears silently. Recommend either: (a) a one-line comment at the
`SPI.begin()` call site in `storage.cpp` noting the LCD also depends on this
running first, or (b) calling `SPI.begin()` explicitly and unconditionally in
`Hal::init()` before `Storage::init()`/`Display::init()`, so the LCD's
correctness doesn't ride on the SD driver's implementation detail.

## 3. `esp-idf#18625` + `esp-usb#371` citation pairing overstates the case

**Where:** `AGENTS.md`, "The ESP32-C5 cannot be a USB keyboard" section.

**The claim:** both issues together show Espressif marked C5 TinyUSB device-mode
"Won't Do."

**What's actually true:** `esp-idf#18625` does carry `Resolution: Won't Do` and
is the right citation for "no OTG device-mode software path, and it's not
coming." `esp-usb#371` is a different, narrower issue (a TinyUSB MSC build error
on an IDF 6.0-beta1 snapshot) closed `Resolution: Done` — i.e. it was *fixed*,
which if anything is evidence of incremental C5 USB work happening, not a second
data point for "won't do." Citing them together makes the "definitely blocked"
case look more solid than the primary sources actually support.

**Recommended fix (docs-only, safe for any agent):** in `AGENTS.md`, cite only
`esp-idf#18625` for the "won't do" claim; if `esp-usb#371` is kept at all, label
it accurately as "a related but separate, resolved MSC issue — evidence C5 USB
support is still evolving, not evidence of a second won't-fix."

## 4. README's hardware-status banner didn't match the current branch — FIXED 2026-08-30

**Where:** `README.md` top banner.

**Resolution:** confirmed against `.hermes/plans/2026-08-29_c5-hardware-dossier.md`
and `.hermes/plans/2026-08-28_093918-tdongle-c5-bringup.md` (untracked local
files — see #6) that hardware bring-up genuinely happened: build, flash, boot,
SD, LCD, and the BOOT button are all confirmed working on a real T-Dongle-C5;
the WiFi C2 REST API is implemented and the server starts but hasn't been
exercised end-to-end; the LED is currently broken (see #1). README's banner and
`AGENTS.md`'s Status section were rewritten to reflect this, with a per-subsystem
table instead of a single "untested" claim, so this doesn't silently go stale
again the next time only one subsystem's status changes.

## 5. `AGENTS.md` names GPIO5 "MTDI" — it's MTDO

**Where:** `AGENTS.md`, config.h comment in `firmware/src/config.h`
(`CFG_RELEASE_JTAG_LED_PINS`).

Per the ESP32-C5 datasheet's own strapping-pin table and the ESP-IDF JTAG-pin-map
guide (both independently agree): GPIO2=MTMS, GPIO3=MTDI, **GPIO4=MTCK,
GPIO5=MTDO**. The LED's clock pin (GPIO4) is correctly MTCK; the data pin
(GPIO5) is MTDO, not MTDI. Doesn't change any pin number or behavior — purely a
label fix, safe for any agent to make in a docs-only commit. See
`hardware-esp32-c5.md` for the full table and sources. **Fixed 2026-08-30** in
`config.h` and `hal.cpp`'s comments.

## 6. RESOLVED 2026-08-30 — `.hermes/plans/*.md` is now tracked, sanitized

**Where:** `.hermes/plans/2026-08-29_c5-hardware-dossier.md` and
`.hermes/plans/2026-08-28_093918-tdongle-c5-bringup.md`.

**Original problem:** these two files are, by a wide margin, the best-sourced
hardware evidence in this entire project — a chronological table of every LED
experiment with exact settings and observed results, the exact DTR/RTS reset
gotcha, exact serial log lines, exact tokens and card sizes seen. Everything in
this file's #1 and the README/AGENTS.md status updates from 2026-08-30 is
downstream of these two files. But `.gitignore` excluded `.hermes/` entirely
("Agent state (local to each clone)"), so this evidence never travelled with
the repo — a fresh clone, a different machine, a different agent session
with no filesystem access to this one, or the VM being rebuilt, all lost it
permanently.

**Resolution:** decided (2026-08-30, this session, on the repo owner's
explicit instruction) to track `.hermes/plans/` specifically, while leaving
the rest of `.hermes/` (sessions, cache, other per-clone state) gitignored —
`.gitignore` now reads `.hermes/*` + `!.hermes/plans/`. Before tracking,
both files were checked for secrets/credentials and sanitized: five live
WiFi C2 auth tokens redacted (they regenerate every boot and were already
stale by the time this was done, but `CONTRIBUTING.md` bans committing
tokens on principle), and local-machine specifics genericized (home
directory path revealing the username, a libvirt VM/hostdev filename, an
SSH key filename). **Kept deliberately**: the physical unit's MAC address
(`38:44:BE:BC:FA:C4`) — not a credential, and useful for the still-open gap
noted in #1's addendum (whether later hardware sessions are testing the same
physical board). No actual passwords or key material were found in either
file; the WiFi password appearing in example commands was the project's old
public lab default, since rotated and moved out of the repo into the
gitignored `config_secret.h` (see `config_secret.h.example`).

**Standing discipline, unchanged by this resolution:** promoting durable
findings out of a plan file into this knowledge base (or `AGENTS.md`, or a
commit message) before the plan file is superseded is still the rule per
`agent-playbook.md` — tracking the plan files themselves doesn't replace
that, it's a safety net under it. And: **any future edit to these two files
must be checked for tokens/paths/secrets before committing**, the same way
this pass was — tracking them means anything added from here on ships to
GitHub, not just to this one machine.

## 7. LED — two minor, non-blocking follow-ups (added 2026-08-30, after #1 was resolved)

Neither of these blocks anything — the LED and LCD both work (see #1). They're
recorded so a future session doesn't rediscover them from scratch.

### 7a. Backlight vs. LED brightness tuning (per-unit, cosmetic)
`CFG_LCD_BL_LEVEL` (config.h) is 180 and `HAL_APA102_BRIGHTNESS` (hal.cpp) is
10 (the vendor's value). On the unit tested 2026-08-30 (MAC `F9:3C`) the LED
read clearly as teal with the backlight on — no washout. But housings/diffusers
vary between units, so if the LED ever looks *washed to white* against the
backlight on a different board, the fix is one of: lower `CFG_LCD_BL_LEVEL`
(dimmer LCD) or raise `HAL_APA102_BRIGHTNESS` (0–31; brighter LED). This is
cosmetic tuning, not a bug — and note the *old* "backlight washes the LED
white" claim was actually the pin bug (#1), so don't over-rotate on it.

### 7b. `Hal::ledRefresh()` covers LCD paints but not SD reads
On the C5 the APA102 shares the SPI bus (GPIO2/6), so any bus traffic clocks
garbage through the CS-less LED. `Display::update()` calls `Hal::ledRefresh()`
after an LCD paint to re-latch the colour, which covers the steady-state case
(dashboard updates). It does **not** fire after SD reads/writes on the same bus
(`Storage::` calls) — e.g. loading a payload after a button press briefly
corrupts the LED until the next status change re-sends it. In practice this
self-corrects within a loop iteration (the next `Display::update()` status call
re-sends), so it's a sub-visible transient, not a stuck wrong colour. If it ever
becomes visible: call `Hal::ledRefresh()` after `Storage::` bus operations too,
or centralise all shared-bus access behind one wrapper that refreshes the LED on
exit. Low priority; left as-is because the visible symptom is nil today.

## 8. Intermittent cold-boot panic on the C5 — RESOLVED 2026-09-08 (from-source build); issue #18

Seen during Phase 2d (AP-down PMF / PMFDOWN) bring-up on `/dev/ttyACM0`: about
**1 in ~4 app-resets** panicked at boot (CPU register dump + stack backtrace)
instead of printing the normal banner; the other resets booted cleanly and the
panic did not recur once settled. It reproduced on **both** the pre-fix and
post-fix Phase 2d images, so it is **not** caused by the AP-down PMF work — it
reads as a pre-existing early-init instability, surfaced here only because this
session reset the board many times in a row.

**Evidence.** Panic boot reached HAL + SD init (`[SD] OK: SD (30436.5 MB)`) then
dumped registers/stack; the serial port then dropped (`Resource temporarily
unavailable` = USB CDC re-enumerated). The immediately-following reset, same
firmware, booted clean through `[C2] Starting SoftAP... OK — 192.168.4.1` →
`[MAIN] Boot complete`.

**Lead / caution.** `E (…) MSPI Timing: Failed to allocate dummy cacheline for
PSRAM memory barrier!` prints on **every** boot, clean ones included — so it is
probably *not* the panic trigger despite being the scariest-looking line; don't
over-rotate on it (cf. #1's "backlight washes the LED white" red herring). The
fault is early (around/after HAL+SD, before/at WiFi C2 start), consistent with
PSRAM/MSPI init timing rather than anything in the recon/PMF path. Next step: a
decoded backtrace — build with symbols and run several raw dumps through the
`esp32c5` exception decoder / `addr2line` to localize the faulting frame.

**Severity: low, non-blocking.** Device recovers on the next reset and runs
normally (Phase 2d validated fine across it). Full write-up and repro in
issue #18.

> **RESOLUTION (2026-09-08, live hardware, `/dev/ttyACM0`).** Root-caused and
> fixed. The trail:
> - **Reproduction sharpened:** the panic is a **cold-boot** phenomenon. On a
>   true power-cycle (physical unplug → cool → replug) the precompiled build
>   panics *repeatedly*, producing a visible **reboot loop** (LCD/LED flashing)
>   until timing settles into a clean boot; warm RTS resets almost never trigger
>   it (0 in ~75 warm resets). The dump was uncapturable from the guest: the
>   USB-Serial-JTAG re-enumerates on every reboot, so the loop's dumps scroll
>   past before the port re-attaches — the capture only ever caught the settled
>   clean boot.
> - **PSRAM ruled out.** The scary every-boot MSPI cacheline line was a red
>   herring (as warned above). Disabling SPIRAM (`CONFIG_SPIRAM=n`) removed that
>   line but the cold reboot loop **persisted** — so marginal PSRAM timing was
>   not the cause. (Disabling SPIRAM also regressed WiFi init, `0x3001`, so it
>   is not a usable change anyway; PSRAM stays on.)
> - **Localized to the precompiled core.** The from-source and precompiled
>   sdkconfigs are byte-identical (0 deltas but SPIRAM), yet a **from-source**
>   build of the *same* config (fresh IDF 5.5.5) does **not** reproduce the loop
>   — ~15 cold cycles clean vs the precompiled build looping. So the fault lives
>   in the **precompiled arduino-esp32 core binaries** for the C5 (a very new
>   chip on the pioarduino fork), not in our code or config. The one garbled
>   dump we did get pointed at `esp_phy_load_cal_data_from_nvs` + heap during
>   WiFi/PHY init, consistent with an early-init bug in that prebuilt code.
> - **Fix:** switch the `T-Dongle-C5` env to a **from-source build** via a
>   `custom_sdkconfig` entry in `platformio.ini` (any entry triggers pioarduino's
>   from-source path; we pin the default recover-on-panic behavior as the
>   trigger). Hardware-validated: cold reboot loop gone across the validation
>   cycles, WiFi/SoftAP + SCAN/PMF/PMFDOWN all intact. **Cost:** C5 builds are
>   now much slower and pull `framework-espidf` on first build for anyone
>   cloning — documented in `AGENTS.md` and `platformio.ini`.
> - **Not captured:** an exact decoded MEPC. The catch-22 (only the precompiled
>   build reproduces it, and it can't be made to halt-on-panic without going
>   from-source, which then doesn't reproduce it) makes the faulting instruction
>   uncapturable without JTAG/gdbstub — not pursued, as the from-source build
>   sidesteps the bug entirely.

## 9. RESOLVED 2026-11-05 — root cause was the Arduino WiFi stack, not the C2 HTTP layer; fixed by a from-source ESP-IDF SoftAP env (`T-Dongle-C5-idf`)

> **Resolution summary (read this first; the investigation log below is kept
> as-is for the evidence trail).**
>
> **Root cause:** the arduino-esp32 WiFi wrapper plus the prebuilt Arduino
> WiFi/PHY/MAC blobs — **not** the C2 HTTP layer / `WebServer`/`NetworkClient`.
> Aligning sdkconfig toward the working minimal IDF demo did not fix SoftAP
> (and instead Guru-panicked Arduino at `esp_wifi_start`); the same board on
> plain from-source `esp_netif` + `esp_wifi` associates and serves HTTP
> indefinitely.
>
> **Fix:** main-tree PlatformIO env `T-Dongle-C5-idf` (`framework = espidf`,
> no Arduino core), sources under `firmware/src/idf_softap/`, SPIRAM off,
> 2 MiB factory partition. Builds clean; 145/145 WiFi/LWIP sdkconfig keys
> byte-identical to the hardware-passed hybrid.
>
> **Flashed and SoftAP-regressed PASS on win11+Hama (2026-11-05):** DHCP
> 192.168.4.6; HTTP 200 on `/`, `/raw`, `/api/status`, `/api/payloads`; recon
> scan + enum + capture held SoftAP; authenticated pcap GET 200 with libpcap
> magic/Content-Type; 401 without auth; 404 for missing name. Device left on
> that image.
>
> Arduino env `T-Dongle-C5` is unchanged and kept as the #9 reproducer.
>
> Detail: `issue-9-softap-findings.md` + `issue-9-RESUME.md`.

**Original opening as written** — `## 9. OPEN — C2 REST API's HTTP layer intermittently fails for a real external WiFi client; root cause not yet found, and this dev VM lacks the tooling to find it`

**Where:** `firmware/src/c2/web_server.cpp` (`WebServer::handleClient()` via the
vendored arduino-esp32 `WebServer`/`NetworkClient` libraries), C5 SoftAP.

**Why this matters more than it might look:** every prior "C2 REST API verified
end-to-end on hardware" claim in this file's Status section (the 12/12 self-test,
2026-08-30) used the device's own `127.0.0.1` loopback — never a real external
WiFi client. AGENTS.md already flagged this gap ("Still pending: a true
over-the-air pass... loopback covers the HTTP+handler+interpreter+SD path but
not the radio/DHCP path"). This entry is the first time that pass was actually
attempted (2026-09-16, a Win11 VM joined the SoftAP as a real WiFi client and
drove the API with `curl`), and it does not work reliably.

**Symptom, in two different failure modes seen on the same board in the same
session, separated by minutes with a stable link the whole time:**
1. TCP connects, the full HTTP request is sent and received, the server-side
   handler runs to completion and `WebServer::send()` returns with no error —
   and the client still receives zero response bytes and hangs indefinitely.
2. TCP itself never establishes — `curl` reports a connect-phase timeout before
   getting anywhere near HTTP. Observed in bursts: one clean 4-minute run of 60
   consecutive `/api/status` requests, 1 request every ~4s, was **100% connect
   timeouts** (`curl -w '%{http_code}'` → `000` every time).

**What was ruled out, each with hardware evidence, before concluding this is a
genuine unresolved bug rather than an environment mistake:**
- **Client-side networking.** DHCP handed the Win11 VM a correct
  `192.168.4.0/24` address, `ping 192.168.4.1` succeeded every time, and `curl
  -v` showed a real completed TCP handshake in failure mode 1 above. Not a
  routing/DNS/firewall problem.
- **Response size.** The dashboard's ~7KB HTML page was suspected first (this
  project has a documented history of large-response TX stalls over this
  SoftAP, see the recon pcap-download comment in `web_server.cpp`); a bare
  ~100-byte `/api/status` JSON response fails identically.
- **WiFi link/association dropping.** `WiFi.onEvent()` handlers for
  `ARDUINO_EVENT_WIFI_AP_STACONNECTED`/`_STADISCONNECTED` (now a permanent
  addition to `web_server.cpp` — logs the station MAC and, on disconnect, the
  802.11 reason code) never fired a disconnect during either failure mode; the
  LCD's `Clients:` row stayed at `1` throughout; and a ~4-minute promiscuous
  capture of 802.11 management frames spanning a failure window (via Recon's
  existing Phase 1 capture, triggered over serial to sidestep the very HTTP
  path under test) shows **zero** deauth/disassoc frames involving the
  station's MAC. The link is solid at the radio-association level in both
  failure modes.
- **Main-loop / CPU stalls.** A temporary loop-duration watchdog
  (`millis()` delta each `loop()` iteration, logging anything over 100ms) was
  flashed and run through both a successful request and a failing one:
  **zero stalls** in either case. `WebServer::handleClient()`'s state machine
  is being pumped promptly and often; the failure is not "the sketch is busy."
- **Recon capture interference.** The LCD's `Recon:` row read `off` during
  the 100%-failure 4-minute window — no capture was silently running and
  monopolizing the radio.

**A real, separate, confirmed bug found along the way (not the cause of the
above, but worth fixing on its own):** `Recon::stopCapture()` blocked
`loop()` for **3.2 seconds** flushing 1642 buffered packets to SD in one shot,
and the serial `DUMP` command blocked it for **~1.4 seconds** reading a file
back. Both were caught by the same loop-duration watchdog. Neither happened
during the specific windows that produced the HTTP failures above (no capture
was active then), so they're independent findings, not the same bug — but a
multi-second unyielding block anywhere in `loop()` is a real problem for a
cooperatively-scheduled sketch that also has to service `WebServer`, the
interpreter, and OS-detection timing, and should be fixed (chunk the SD
flush/read across multiple `loop()` iterations rather than doing it in one
blocking call) independent of this investigation.

**What could not be determined, and why — a real tooling gap, not a dropped
thread:** whether the AP's own response frames ever leave the radio. This
project already has a working promiscuous 802.11 capture (Recon Phase 1), and
the natural next step was to capture the AP's own channel during a failing
request. That capture ran successfully (1642 packets, ~4 minutes) but
contained **zero** frames from the AP's own SSID/BSSID at all — not because
the AP wasn't transmitting, but because a single-radio promiscuous sniffer
cannot see its own device's outgoing transmissions (it never saw its own
beacons either, which should number in the thousands over that window if it
could). Answering "does the response frame actually leave the radio" needs an
**independent second radio** in monitor mode watching the AP's channel from
outside — a second WiFi adapter on another machine, or a dedicated sniffer.
This dev VM has no WiFi radio at all (the reason the over-the-air test needed
a separate Win11 VM as the client in the first place), and no such
external-capture setup exists yet for either machine.

**Leading hypothesis, unconfirmed:** something in this lwIP build's handling
of the SoftAP's own interface/IP, specific to real external-station traffic,
possibly related to the already-documented "`192.168.4.1` loopback isn't
routed on this lwIP build" quirk (Status section, Phase 2b) — the self-test
had to use `127.0.0.1` to route around *that* quirk, and it's plausible both
symptoms share a root cause in how this build's netif/lwIP config handles the
AP's own address, now showing up from the other direction (a real external
peer talking to that address) rather than the device talking to itself. This
is a hypothesis, not a finding — nothing above actually tested it.

**Recommended next steps, in order of cost:**
1. Fix the confirmed SD-blocking bug (`Recon::stopCapture()`/`DUMP`) — cheap,
   independent, real.
2. Get a second radio in monitor mode (any spare WiFi adapter, on the Win11
   VM or elsewhere) watching the AP's channel during a repeat of this test —
   the one piece of evidence that would actually distinguish "frame never
   transmitted" from "frame transmitted but not received/acked" from "frame
   received but dropped somewhere in the client's own stack" (the last of
   which curl's `-v` output already argues against, but an independent
   capture would confirm outright).
3. If a second radio isn't available soon, next-cheapest is ESP-IDF-level
   instrumentation this session's Arduino/Serial toolchain can't reach:
   `lwip_stats` (needs `CONFIG_LWIP_STATS=y` in the sdkconfig this project
   already builds from source, so it's reachable) for TX/RX/drop counters at
   the netif and TCP layers, read out over serial.
4. Do not re-attempt this by adding more `Serial.printf` diagnostics at the
   `WebServer`/`NetworkClient` application layer — this session already
   proved every layer down to and including the app's own `send()` call
   behaves correctly and reports success. Further app-layer prints cannot
   see past that boundary; the next useful signal has to come from either
   outside the radio (step 2) or inside the IDF network stack's own counters
   (step 3).

### Update 2026-09-17 — fix attempted (step 3 above), disproven, bug re-characterized, and a real second-radio capture attempt hit a hard tooling wall of its own

**Fix attempted, per step 3 above:** three targeted `custom_sdkconfig` additions
in `platformio.ini` — `CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=32` (8→32, headroom
against the TX-buffer-exhaustion mechanism a C5-specific community report
showed), `CONFIG_LWIP_STATS=y` (instrumentation), and
`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y` (was OFF — closes a real gap where
a spinning task on this single-core chip would starve the idle task with no
watchdog catching it). Web research first (a background agent search of
Espressif's GitHub issues, ESP-IDF changelogs, and community reports) found no
exact confirmed match for this symptom anywhere public — closest leads were a
resolved-but-undiagnosed STA-mode analogue (`esp-idf#14016`), a C5-specific but
undiagnosed community report showing the same `wifi:packet allocation failed`
mechanism during an AP hang (`GhostESP-Revival/GhostESP#382`), and confirmation
that `CONFIG_LWIP_TCPIP_CORE_LOCKING=y` (which this project's sdkconfig has on,
not the ESP-IDF default) is the precondition for a real, Espressif-acknowledged
locking bug (`esp-idf#9908`) — not touched here given the risk of an
Arduino-wide default no session had time to fully de-risk.

**A second, independent radio finally became available mid-investigation** (an
Alfa AWUS036AU/RTL8812AU passed through to this dev VM) — the first real chance
to see over-the-air traffic from *outside* the C5's own radio, which a
same-device promiscuous capture structurally can't do (confirmed earlier: zero
of the AP's own beacons ever showed up in its own capture). Getting a clean
test out of it took far longer than expected and is itself worth recording:

- Two of this session's own `wpa_supplicant` instances ended up fighting over
  the same interface more than once, and a `pkill -f wpa_supplicant` matched
  and killed the *invoking shell's own eval string* (which contains the literal
  text "wpa_supplicant") rather than the target process — silently truncating
  command output and producing a string of confusing, unrelated-looking
  failures for a good stretch of the session before the mechanism was
  identified. Lesson for next time: `pkill`/`pgrep` by exact PID or by bare
  process name (no `-f`) when the pattern could plausibly appear in your own
  command line.
- NetworkManager (which owns this VM's WiFi by default) and a manually-run
  `wpa_supplicant` both trying to manage the same interface caused the same
  class of flapping.
- The client radio itself needed a full kernel module reload
  (`rmmod`/`modprobe rtw88_8821au`) at one point to clear a stuck state a
  simple `usbreset` and interface down/up didn't fix.
- The C5's own USB-Serial-JTAG console independently dropped from this VM more
  than once during the session (device stayed on the USB bus per `lsusb` but
  no `/dev/ttyACM*` node, zero configured interfaces in sysfs) — fixed each
  time by a host-level USB passthrough detach/reattach, not by anything
  software-fixable from inside the guest. Not established as related to the
  firmware bug; recorded because it cost real time and could recur.

**Once a genuinely clean, single, patient, serial-verified test was achieved**
(fresh board reboot, confirmed clean boot log, single `wpa_supplicant`
instance, real hardware MAC, 60s+ left undisturbed before checking), the result
was decisive and — importantly — more precise than anything gathered before:

- **The very first HTTP request after boot succeeded completely.** A real
  `curl` from the external Linux client got a clean `HTTP/1.1 200 OK` with the
  correct JSON body (`{"state":"complete","clients":1,"ap_ip":"192.168.4.1"}`)
  — the first fully successful external-client request in this entire
  investigation, across every session that has touched this bug.
- **Every request after that first one failed completely** — 5 consecutive
  `curl` attempts immediately following all timed out at the TCP-connect stage
  (`HTTP 000`), with the 802.11 association still healthy throughout
  (`iw ... link` showed "Connected" with climbing RX byte counters).
- So the fix did **not** resolve the bug, but it clarified its actual shape:
  this is not "randomly sometimes works, sometimes doesn't" — on a clean run it
  is **exactly one success, then a permanent wedge**, every time tested this
  way. Whether the earlier sessions' apparent intermittency (sometimes several
  requests succeeded before failing, sometimes none did) was real variation or
  itself an artifact of unclean test conditions (the same class of
  double-process/stale-association confounds documented above) is now an open
  question in its own right — this was the first time the test was run with
  this level of hygiene.
- **`LWIPSTATS`** (new permanent console command, `firmware/src/console/`,
  calls lwIP's own `stats_display()`) pulled immediately after the wedge, on
  this exact reproduction, showed: **zero** drops/memory errors/protocol
  errors anywhere — `LINK`, `ETHARP`, `IP`, and `TCP` sections all clean — and
  a **cached, still-valid ARP entry** for the client (`ETHARP cachehit: 380`,
  `xmit: 1`/`recv: 1` — one real ARP exchange ever, then 380 reuses). This
  rules out lwIP-level resource exhaustion (pbuf/memp pool exhaustion, the
  buffer-bump fix's own working theory) as the mechanism — lwIP's own
  accounting says nothing is wrong on its side. `TCP xmit: 380` for what
  should be a handful of request/response/retry exchanges is itself notable
  and unexplained.

**Where this leaves the investigation:** the remaining plausible mechanisms are
now narrowed to two, neither visible to lwIP's own stats: (1) something in the
WiFi driver's own internal state, below lwIP, that a "clean" lwIP dump
can't see; or (2) something in the Arduino `WebServer`/`NetworkClient`
library's own connection-accept state machine that gets stuck specifically
*after* successfully completing and closing one connection (consistent with
this session's very first diagnostic pass, which showed the handler
sometimes runs and `send()` returns without error — i.e. the app layer
believes it succeeded — yet nothing reaches the client). A genuinely
independent over-the-air capture (the Alfa, in monitor mode, watching the
AP's channel from outside during a live reproduction) would settle (1)
outright and was the natural next step, but did not happen this session — by
the time the client radio was stable enough for a controlled test, the
priority was validating the fix build, and session time ran out first. That
remains the single highest-value next step, now that a second radio actually
exists in this environment and the process pitfalls above are documented.

**Kept from this round, not reverted:** the three sdkconfig changes above
(harmless, and the WDT idle-check is real resilience independent of root
cause) and the `LWIPSTATS` console command (already proved its worth —
keep using it on any future reproduction attempt, ideally *before* assuming
a buffer/memory theory again, since this round's data already argues against
one).

### Update 2026-09-17 (second session, same day) — root cause LOCALIZED with direct evidence: the AP→station DOWNLINK TX path in the WiFi driver, below lwIP, silently drops ~99% of the frames lwIP hands it. lwIP RX (uplink) is fully healthy; the Arduino WebServer/accept theory is effectively ruled out.

**Headline:** on a rigorously clean, both-sides-confirmed reproduction, the C5's
own lwIP reported transmitting **217 TCP segments** (SYN-ACKs + retransmits)
with **zero** TX errors (`drop`/`memerr`/`err` all 0), while an independent
client-side packet capture proves the client received **exactly 2 frames** from
the AP across the entire test (one ARP reply, one lone SYN-ACK). Uplink is
perfect: lwIP *received* all 30 of the client's TCP SYNs (`TCP recv: 31`). The
loss is entirely on the **AP→station downlink, between lwIP's tx handoff and the
air** — the one layer `LWIPSTATS` cannot see, which this session's prior update
named as remaining lead #1. It is now the confirmed locus.

**Why this was done without the independent-monitor capture the previous update
called for, and why the substitute is actually more direct for this question:**
this dev VM has exactly **one** WiFi radio (the Alfa/RTL8812AU is `phy2`, the
only phy present), and the SoftAP is **WPA2-PSK** (SSID `Lily`, ch6). Monitor
mode and being the test client are mutually exclusive on one radio, and you
cannot inject a WPA2 data frame from monitor mode without the PTK — so the
"second radio in monitor mode during a live client request" test is not
performable in this environment as-is. Instead the AP's *own* transmit
accounting (`LWIPSTATS` TCP/IP/ETHARP `xmit`) was compared against what the
client's stack actually received (`tcpdump` on the client's managed-mode netdev,
below its IP layer). That pair answers "did the AP emit a response, and did the
client receive it" more directly than a sniffer would: it reads lwIP's intent
(217 sent) against ground-truth delivery (2 received). See the residual gap the
monitor capture *would* still close, at the end.

**Reproduction conditions (the cleanest yet, and both-sides-confirmed):**
- Board freshly re-enumerated/rebooted (LWIPSTATS baseline was near-zero:
  `ETHARP/IP/TCP recv` all 0), `/dev/ttyACM0`, iSerial `38:44:BE:BC:F9:3C`.
- **Association confirmed healthy from BOTH ends, for the whole test:** client
  `iw link` steady at -40 dBm for 60 s undisturbed before starting and still
  `Connected` after; the C5's own `WiFi.onEvent()` logged exactly one
  `station connected: 00:C0:CA:B1:71:6F (aid=1)` and **zero** disconnects
  across the entire run. Static IP `192.168.4.60/24`, client `power_save off`,
  single `wpa_supplicant` instance, NetworkManager set `unmanaged` on the iface.
- A serial monitor logged the C5 continuously; requests were driven from the
  Linux client with `curl -w`.

**The decisive data (one clean run, three `LWIPSTATS` pulls + one client pcap):**

| Layer  | after assoc | after 1 ping | after 6× `curl /api/status` | client actually RXed |
|--------|:-----------:|:------------:|:---------------------------:|:--------------------:|
| ETHARP xmit/recv | 0 / 0 | 1 / 1 | 4 / 4 | 1 ARP reply |
| IP xmit/recv     | 0 / 0 | 2 / 2 | **219 / 33** | — |
| TCP xmit/recv    | 0 / 0 | 0 / 0 | **217 / 31** | **1 SYN-ACK** |

- **Uplink (client→AP) is fully healthy.** lwIP received every SYN: `TCP recv:
  31` ≈ the 30 SYN+retransmits the client pcap shows it sent. So the driver's
  *receive* path delivers the station's frames to lwIP correctly. (This
  overturns this session's own first, discarded run — see "false start" below.)
- **Downlink (AP→client) is where it dies, silently.** lwIP emitted `TCP xmit:
  217` (SYN-ACKs and their retransmits) with a clean `drop:0 memerr:0 err:0`
  and `cachehit: 31` — from lwIP's and the app's point of view every send
  succeeded. The client pcap, taken below its own IP stack, received **2 frames
  total from the AP** the entire test: the initial ARP reply
  (`10:02:24`, 2 ms after the who-has) and a single stray SYN-ACK (`10:02:53`).
  ~2 of ~219 attempted downlink frames arrived. curl reported `code=000
  conn=0.000000` for all 6 requests (the TCP handshake never completed).

**What this RULES IN (with direct evidence, not inference):**
- The fault is in the **WiFi driver's AP→STA transmit path, below lwIP** —
  `esp_wifi_internal_tx()` (or the layer under it) accepts frames from lwIP,
  returns success, and does not deliver them to an associated station. lwIP's
  own accounting is clean precisely because the failure is beneath it. This is
  the mechanism the buffer-bump and lwIP-stats work could never see, now shown
  directly by the xmit-vs-delivered gap.
- The "works once, then wedges" shape is **downlink-frame-granular**: the very
  first downlink frame (the ARP reply) got through, then delivery collapsed;
  one later SYN-ACK slipped out. Consistent with earlier sessions' "first
  request succeeds then wedges," now located at frame-TX delivery rather than
  at HTTP or TCP.

**What this RULES OUT (or strongly demotes):**
- **The Arduino `WebServer`/`NetworkClient` accept-state-machine theory (prior
  lead #2).** The frames that fail to leave are lwIP's *own* SYN-ACKs, generated
  by the TCP stack with no WebServer involvement — the handshake never completes,
  so the WebServer never receives a connection to mishandle. The failure is
  below TCP entirely. `NetworkClient` is not the cause.
- **A TCP-specific bug.** ICMP echo replies fail the same way (2 echoes sent by
  the client, 0 replies received), as does most ARP. It is general downlink, not
  a TCP quirk.
- **lwIP resource exhaustion** — reconfirmed: `drop:0 memerr:0` everywhere, TX
  path never reports back-pressure.

**Honest caveats and a discarded false start:**
- **A first run this session is discarded as inconclusive.** Before confirming
  association health from the C5 side, an earlier attempt showed `ETHARP recv:
  0` and total failure — but that run's client link dropped to `Not connected`
  moments later, i.e. the association was actually unstable (the documented
  rtw88/wpa_supplicant/NetworkManager flapping). Its "driver drops uplink"
  reading is an artifact of a half-up link and is superseded by the
  both-sides-confirmed run above, where uplink is demonstrably fine. Recorded
  so the contradiction isn't rediscovered as a mystery.
- **lwIP's ICMP counters looked unreliable** in this build (`ICMP recv: 0` even
  though `IP recv` accounts for the 2 echo requests); the conclusion rests on
  the TCP counters, which are internally consistent (`recv 31`/`cachehit 31`,
  `xmit 217`) and corroborated by the pcap.
- **The one thing still not physically proven, and the sole remaining job for an
  independent monitor radio:** whether those 215 undelivered frames are dropped
  *inside the driver before the PHY transmits them*, or are transmitted on-air
  but dropped by the client's own mac80211 on receive. The evidence makes the
  latter implausible — the client receives this AP's beacons continuously (RX
  byte counters climb), received its ARP reply and one SYN-ACK cleanly at
  -40 dBm, so 215 consecutive same-link RX drops would be extraordinary — but
  only a second radio in monitor mode on ch6, watching the AP from outside
  during a live wedge, closes it outright. Single-radio + WPA2 still blocks that
  test in this VM; it needs a second radio *and* a separate client device.

**Recommended next steps, revised:**
1. Treat this as an ESP-IDF/arduino-esp32 WiFi-driver downlink-TX bug on the
   ESP32-C5 SoftAP, not an application- or lwIP-level bug. Search/report
   upstream with this specific signature: *SoftAP, associated station stays
   connected, `esp_wifi_internal_tx()` returns OK, station receives almost no
   downlink frames.* (The `GhostESP-Revival/GhostESP#382` "packet allocation
   failed" C5 report and `esp-idf#14016` STA-mode analogue remain the closest
   public leads; this is stronger evidence than either.)
2. Instrument the driver TX path directly, below lwIP: log the return of the
   raw `esp_wifi_internal_tx()`/`wifi_transmit` call and, if reachable, the
   WiFi driver's own low-level TX free-descriptor / `wifi:packet allocation`
   state at the moment of the wedge — this is the layer that now needs eyes,
   and it is below where `LWIPSTATS` and app-layer prints can see.
3. Only if driver instrumentation is inconclusive, do the independent
   monitor-radio capture (needs a *second* radio plus a separate client) to
   split "dropped before PHY" from "TXed but lost" — now a tie-breaker, not the
   primary lead.

**State left on the bench:** board associated and idle at `/dev/ttyACM0`; the
raw capture, three LWIPSTATS blocks, and event log from this run are in the
session scratchpad. No firmware changed this session (LWIPSTATS + onEvent were
already committed); nothing to revert.

### Update 2026-09-17 (second session, cont.) — driver TX-done instrumentation added, flashed, and confirmed working; wedge measurement blocked by the client adapter (rtw88) failing its WPA2 4-way handshake after this session's USB churn

Built the next step the update above called for — direct visibility into the
WiFi driver's TX path, below lwIP — as two permanent serial commands (commit
`65e0222`):
- **`TXSTATS`** dumps the driver's per-frame TX-done callback
  (`esp_wifi_set_tx_done_cb`) counters, per interface: `ok` = MAC transmitted
  **and** the peer ACKed, `fail` = transmitted, not ACKed. This is the layer
  `LWIPSTATS` cannot see; it splits the three remaining possibilities — (a)
  frames never reach the MAC (counts stay ~0 while lwIP `xmit` climbs), (b) MAC
  transmits but station never ACKs (`fail` climbs → below the driver's TX
  handoff), (c) transmitted + ACKed (`ok` climbs → loss is on-air/client-side,
  the only case still needing a monitor radio).
- **`TXRESET`** zeroes the counters so one request window can be isolated.

**Confirmed working on hardware:** flashed to the C5, `tx_done_cb register: ok
(0x0)` at boot, and a first live read returned `AP(if1) ok=5 fail=0` for the
frames the AP sent during a client's association attempt (EAPOL). So the
instrumentation is functioning and already gave one real data point: **the AP's
early/handshake downlink frames transmit and are ACKed** — consistent with the
"works at first, then wedges" shape, where the wedge would show up on the
later data-path frames. The decisive full measurement (TXSTATS read at a live
HTTP wedge, cross-checked against LWIPSTATS + client pcap) is **not yet
captured** — see the blocker.

**Blocker (environmental, not the firmware bug):** after this session's heavy
USB churn — the C5's own USB-Serial-JTAG console needed a vid:pid `usbreset` to
come back (the documented "node present, no data" state), and the Alfa
re-enumerated `wlp3s0u5`→`wlp3s0u4`, `phy2`→`phy0`→`phy1` across resets — the
Alfa/rtw88_8821au client can no longer complete a **WPA2 4-way handshake** with
the C5 SoftAP. It associates at the 802.11 level, then the EAPOL 4-way times
out (`wpa_supplicant reason=15`, "4-Way Handshake failed - PSK may be
incorrect") every time, so the C5 never reaches `AP_STACONNECTED` and no data
path ever comes up. This reproduced across: guest-side `modprobe -r/modprobe
rtw88_8821au` (×2), `usbreset 0bda:0811`, a **fresh C5 reboot** (rules out
AP-side WPA state corruption — a clean AP fails identically), a single
`wpa_supplicant` instance with the correct known PSK (same hash that associated
fine earlier this same day), and NetworkManager confirmed `unmanaged`. The
key that "may be incorrect" is provably correct; this is the adapter's crypto
state stuck, matching the pre-existing note that rtw88 "occasionally needs a
full kernel module reload… usbreset and interface down/up alone aren't always
enough." Here even the module reload didn't clear it — the next lever is a
**host-level USB detach/reattach of the Alfa** (or a physical re-plug / a
different client device), which can't be done from inside the guest.

**So, for the next session (tooling is now in place, just needs a healthy
client):** get the Alfa reattached at the host (or use another WPA2 client),
re-establish the both-sides-confirmed stable association from the prior update,
then: `TXRESET` → drive the request train → `TXSTATS` at the wedge, alongside
`LWIPSTATS` and a client-side pcap. Expected discriminator: if `AP fail` climbs
while `ok` stays flat, the MAC is transmitting data frames the station never
ACKs (loss below lwIP's handoff, at TX/RF); if `AP ok` climbs with the client
still receiving nothing, the frames are ACKed yet lost — the sole case that
would finally require the independent monitor radio.

### Update 2026-09-17 (second session, cont. 2) — the client "4-way failure" carries the SAME downlink-wedge signature; it may BE the bug, not an adapter fault. TXSTATS shows the AP sends EAPOL M1 (ACKed) then never sends M3.

After the Alfa was replugged (client re-enumerated cleanly, `phy4`, new interface,
USB autosuspend disabled) and every guest-side adapter recovery was exhausted —
`modprobe -r/modprobe rtw88_8821au` (×2), `usbreset`, a full USB-driver
`unbind`/`bind` of the adapter, and a fresh C5 reboot+replug — the WPA2 4-way
handshake **still fails identically every time** (`wpa_supplicant reason=15`,
"PSK may be incorrect"). The PSK was then **proven correct**: `wpa_passphrase
Lily jocker-batman-mario` outputs exactly the hash in the client conf and
matches `config_secret.h` (SSID `Lily`, pass `jocker-batman-mario`). So
`WRONG_KEY` is wpa_supplicant's generic guess, not a real key mismatch.

**The new evidence, from the TXSTATS tool built this session, reframes it.**
Polling the AP's driver TX-done counter across a single handshake attempt
(after `TXRESET`) shows the AP-side EAPOL transmit count go `0 → 0 → 0 → 1 → 1`:
the AP transmits **M1 exactly once, it is ACKed (`AP if1 ok=1 fail=0`), and then
nothing more** — no M1 retransmit, and no M3 (the count never reaches 2). Because
`tx_done_cb` only fires when the MAC actually transmits a frame, an M3 that the
driver drops *before* the MAC would produce exactly this reading. That is the
**same "first downlink frame succeeds, the next one wedges" signature** this
whole entry is about — here landing on the 4-way handshake's M3 (the AP's second
downlink frame) instead of on a later HTTP/TCP frame.

**Why this matters:** it suggests the "client can't associate" blocker is very
likely **not** an independent adapter fault but the downlink-TX bug itself,
triggering much earlier than before (after 1 frame, vs after the dozens that
went out in the 2026-09-17 clean run where the 4-way completed and the first
HTTP request even succeeded). Whether the bug's trigger-point genuinely moved
earlier, or this is variance, is unresolved — but the mechanism looks identical.
Two honest caveats: (a) TXSTATS alone can't fully separate "M3 dropped in driver
(downlink bug)" from "client's M2 never reached the AP (uplink) and the AP
simply didn't retransmit M1" — though prior clean-run evidence showed uplink is
healthy; (b) the same USB churn did land on the rtw88 adapter, which has its own
documented flakiness, so an adapter contribution isn't fully excluded.

**Cheapest decisive next test:** join the C5 SoftAP with **any other WPA2 client**
(a phone/laptop). If a known-good client *also* fails the 4-way against this C5,
the adapter is exonerated and this is firmware/driver — pointing the fix
squarely at the ESP-IDF WiFi driver's downlink TX path, consistent with
everything above. If the other client associates fine, the Alfa is the blocker
and needs a host-level (VM-outside) USB reattach. The TXSTATS/LWIPSTATS/pcap
measurement procedure from the previous update is unchanged and ready to run the
moment any client can associate.

### Update 2026-10-30 — reproduced on Win11 + Hama RTL8188EUS (Alfa exonerated); permanent SSH into win11

**Client change (decisive for the adapter question):** SoftAP client was **Win11 VM** with USB WiFi **Hama / Realtek RTL8188EUS (`0bda:8179`)**, not the Alfa/rtw88 stick. OpenSSH Server was enabled on win11 (`trash@192.168.1.153` over br0/Ethernet) so curls were driven remotely from the host without paste-each-time.

**Reproduction (clean SoftAP boot, Wi-Fi stayed `Up` at `192.168.4.50` through the train):**
1. C5 SoftAP `Lily` up (`OK — 192.168.4.1`, `tx_done_cb register: ok`).
2. Win11 associated; `Get-NetAdapter Wi-Fi` = `Up`, IPv4 `192.168.4.50`.
3. `TXRESET` then 4× `curl.exe -m 4 http://192.168.4.1/api/status` from win11 over SSH:
   - `[1] code=000 connect≈0.015s starttransfer=0` (TCP connect attempted, no HTTP body)
   - `[2..4] code=000 connect=0` (connect-phase failure)
4. Immediate `ping 192.168.4.1` from win11: **timeout**, while Wi-Fi adapter still `Up`.
5. `LWIPSTATS` shortly after: `IP xmit:193` / `recv:55`, `drop/memerr/err:0` across sampled sections — same "lwIP believes it transmitted; client receives almost nothing" signature as 2026-09-17.

**What this adds:**
- **Alfa/rtw88 is not required for the wedge.** A second, independent WPA2 client hits the same SoftAP downlink failure → points at **ESP32-C5 SoftAP / WiFi-driver downlink TX**, consistent with the prior localization below lwIP.
- Permanent lab path: `ssh trash@192.168.1.153` from the Alienware host (admin authorized_keys). SoftAP traffic stays on the Wi-Fi NIC; SSH stays on Ethernet/br0.
- Operational note: RTS reset can drop USB-Serial-JTAG mid-boot; wait for `HTTP server started` / `OK — 192.168.4.1` before associating. Do not leave `scripts/serial_monitor.py` running long (busy CPU); use short probe or select()-based quiet logger.

**Still open (unchanged locus):** whether undelivered frames are dropped inside the driver before PHY or TXed but lost — needs TXSTATS read in a tighter window around the first failing request (this session's TXSTATS drains were incomplete / raced with LWIPSTATS). Next: Sherlock/upstream search for C5 SoftAP downlink TX + `esp_wifi_internal_tx` silent success; then tighter TXSTATS harness.


### Update 2026-10-30 (cont.) — `WIFI_PS_NONE` workaround tried, did not fix

Flashed `esp_wifi_set_ps(WIFI_PS_NONE)` immediately after `WiFi.softAP()` (`web_server.cpp` `startSoftApRadio`). Boot log shows `[ps=NONE]` and SoftAP `OK — 192.168.4.1`. Retest with win11+Hama (`Up`, `192.168.4.50`): 7× `curl /api/status` all `code=000` (first TCP connect ~19ms then stall; later connect=0); ping timeout while association stayed Up. Same downlink signature as before. Sherlock workaround #1 (SoftAP PS buffer / #6852/#13198) is **ruled out as sufficient** for this board. Next: sdkconfig dynamic TX buffers + `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n` (Sherlock #3/#4), still citing IDFGH-18499 / esp-idf#19267 as the likely root cause.

### Update 2026-10-30 (cont.) — dynamic TX + AMPDU TX off tried, did not fix

Built and flashed with `platformio.ini` `custom_sdkconfig`:
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y`, `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=64`,
`CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n`, still with runtime `WIFI_PS_NONE` (`[ps=NONE]` on boot).

**Retest (win11 + Hama, after `probe_reset.py` SoftAP `OK — 192.168.4.1`):**
1. Assoc flaky for several minutes post-flash (netsh "success" but adapter `Disconnected`); SoftAP still beaconed (`SSID: Lily`, ch6, ~−32 dBm on Alfa scan).
2. Once associated: Wi-Fi `192.168.4.50`, C5 serial `[C2] station connected: 80:3F:5D:13:3B:34 (aid=1)`.
3. One early `curl http://192.168.4.1/` observed `HTTP/1.1 200 OK` (rare); subsequent trains after reconnect: 5× `code=000` / timeout with 0 bytes, `ping 192.168.4.1` timeout while DHCP address still present or after reconnect still `192.168.4.50` then stall.
4. After a failing train: `TXSTATS` AP `ok=0 fail=0` (or only a few `ok` when a rare response landed); `LWIPSTATS` still shows substantial `IP`/`ETHARP` `xmit` with no lwIP `drop`/`memerr` — same below-lwIP downlink signature.

**Conclusion:** Sherlock workarounds #3/#4 (dynamic TX buffers + AMPDU TX disabled) are **not sufficient** on this C5 SoftAP build (pioarduino / IDF 5.5 Arduino). Issue remains open; locus still SoftAP AP→STA TX below lwIP (IDFGH-18499 / esp-idf#19267 family). Next candidates: try IDF master / newer WiFi lib if available via the Arduino platform, or a minimal SoftAP-only IDF example on the same hardware to separate app vs driver.


### Update 2026-10-30 (cont.) — minimal IDF SoftAP+httpd on same C5: **no wedge** → points at lily_ducky app/stack

**Goal:** separate app-specific SoftAP path from ESP32-C5 / IDF 5.5.5 WiFi SoftAP driver.

**What was flashed (scratch project, lily tree untouched aside from reading SSID from `config_secret.h`):**
- Path: `/home/op/scratch/c5_softap_min` (PlatformIO `framework = espidf`, same pioarduino `55.03.311`, board `Lilygo-T-Dongle-C5` via copied `boards/Lilygo-T-Dongle-C5.json`)
- Stock-style SoftAP + tiny `esp_http_server` on `:80` (`/` and `/api/status` → `{"min":1,"hits":N}`)
- SSID reused as `Lily` (same lab WLAN profile); channel 6; WPA2-PSK; default `WIFI_INIT_CONFIG_DEFAULT()` (no lily app layers, no WebServer.cpp, no PS/`AMPDU`/TX tweaks)
- Boot (ESP-IDF **5.5.5**): `wifi:mode : softAP`, DHCP `192.168.4.1`, serial `SOFTAP_UP` + `HTTPD_UP :80`

**Win11 + Hama RTL8188EUS retest (SSH `trash@192.168.1.153`, Ethernet for SSH):**
1. Assoc: Wi-Fi got `192.168.4.50`; C5 serial `alive sta=1`
2. Train A: 5× `Invoke-WebRequest http://192.168.4.1/` → **all HTTP 200**, ~60–117 ms, `hits` 1→5; ping 4/4 (~2–9 ms)
3. Train B: 20× `http://192.168.4.1/api/status` → **ok=20 fail=0**, `hits` 6→25; ping 6/6
4. Follow-up: hits continued to 28; serial `alive sta=1 hits=28` — no disconnect, no downlink stall in this session

**Conclusion for #9:** On this same T-Dongle-C5 + IDF 5.5.5 SoftAP, a **minimal SoftAP+httpd does not reproduce** the AP→STA wedge seen with lily_ducky under the same client. That strongly points to **lily_ducky app/stack** (Arduino `WebServer` / C2 SoftAP bring-up / concurrent features), **not** a bare ESP32-C5 SoftAP driver failure on IDF 5.5.5. Issue stays OPEN until lily is reduced or the offending path is identified; do **not** treat #9 as fixed. Device currently runs the minimal image — reflash lily firmware when continuing lily debugging.


### Update 2026-10-30 (cont.) — bisect: wedge survives stripping the entire lily app layer AND reverting WiFi sdkconfig to minimal defaults → locus is the Arduino WiFi/WebServer stack, not app/config

**Goal:** given minimal IDF SoftAP+httpd on this same C5 does NOT wedge but lily does, bisect which lily-only difference reintroduces the AP->STA downlink wedge.

**Ranked suspects from the lily-vs-minimal diff:**
1. Concurrent CPU/SPI load on a single-core chip — lily runs a hot `loop()` with no yield, calling `Display::update()` (TFT_eSPI SPI @27MHz) every iteration plus interpreter/recon/button; minimal's app task sleeps 10s, leaving WiFi a near-idle CPU.
2. Arduino `WebServer` (synchronous, polled in loop) + Arduino `WiFi.softAP` bring-up (arduino-esp32 netif/event glue) vs raw `esp_wifi` + `esp_http_server`.
3. lily-only runtime tweaks: `esp_wifi_set_ps(WIFI_PS_NONE)` + `esp_wifi_set_tx_done_cb`.
4. lily-only `custom_sdkconfig` TX overrides (dynamic TX buffers, AMPDU-TX off).
5. PSRAM-on + SD/LCD/bit-banged-APA102 SPI bus sharing.

**Reversible instrumentation added (all off by default, kept for reuse):**
- `CFG_C2_MIN_REPRO` build flag (config.h) + guards in `main.cpp`: skip SD, Display init, recon/interpreter/display/button work; loop() only ticks `C2Server` + `Console` and `delay(2)` (yields the single core).
- platformio envs: `T-Dongle-C5-c2min` (= default C5 WiFi config + stripped app) and `T-Dongle-C5-c2min-stock` (stripped app + custom_sdkconfig reverted to IDF/minimal WiFi defaults: static TX buffers, AMPDU-TX enabled; one benign entry kept so pioarduino still builds Arduino from source, issue #18).

**Test A — `T-Dongle-C5-c2min` (stripped app, default lily WiFi config):** Boot clean (`[ps=NONE] OK — 192.168.4.1`, `tx_done_cb register: ok`, `HTTP server started`; no SD/LCD logs = gating confirmed). Win11+Hama associated (Wi-Fi Up, 192.168.4.50). `TXRESET` then 10x `curl /api/status` -> **10/10 code=000**; `ping 192.168.4.1` **4/4 lost**; association stayed Up. **`TXSTATS`: AP(if1) ok=0 fail=0** (MAC transmitted zero downlink frames the whole train); `LWIPSTATS` ETHARP xmit with 0 drop/0 memerr. -> Suspect #1 (app CPU/SPI load) and the entire app layer (interpreter/SD/LCD/recon) **refuted**.

**Test B — `T-Dongle-C5-c2min-stock` (stripped app + WiFi sdkconfig = minimal defaults):** Boot clean. Win11+Hama associated (Up, 192.168.4.50). `TXRESET` then 10x `curl /api/status` -> **10/10 code=000**; `ping` **4/4 lost**; **`TXSTATS`: AP(if1) ok=0 fail=0** again. -> Suspects #4 (custom_sdkconfig TX/AMPDU) and #3's PS override **refuted** as the cause.

**Conclusion (narrowed, not yet fixed):** With the app layer stripped and the WiFi sdkconfig made equivalent (for the TX path) to the working minimal, the wedge still reproduces identically on the SAME hardware/IDF 5.5.5. The remaining material difference from the non-wedging minimal is the **Arduino WiFi/WebServer stack** itself — `WiFi.softAP()` bring-up via the arduino-esp32 core's netif/event-loop glue, and/or the Arduino `WebServer`/`NetworkClient` TCP path — rather than lily application code, concurrent peripherals, CPU/SPI starvation, PS mode, or TX-buffer/AMPDU config. #9 stays OPEN.

**Cheapest decisive next step:** add a tiny raw-`esp_http_server` route to the Arduino build (keep `WiFi.softAP` Arduino bring-up, bypass the Arduino `WebServer`) and retest: if that route answers while `WebServer` routes wedge, the fault is the Arduino `WebServer`/`NetworkClient` layer; if it also wedges, the fault is in `WiFi.softAP`/arduino-esp32 netif bring-up (point fix at the pioarduino arduino-esp32 WiFi init, e.g. compare its SoftAP esp_netif/mode setup and whether it leaves a STA netif/AP_STA mode active vs the minimal's pure WIFI_MODE_AP). TXSTATS AP ok=0 in both A and B says frames never reach the MAC, consistent with a netif/driver-handoff issue in the Arduino bring-up.

**Device state:** C5 currently runs `T-Dongle-C5-c2min-stock` (a stripped lily image). Reflash `T-Dongle-C5` (full lily) or `c5_softap_min` as needed when continuing. No git commit; branch `docs/wifi-hang-fix-attempt` has the new flag, guards, and two envs (reversible; delete the flag + envs to revert).

### Update 2026-10-30 (cont.) — decisive A/B: raw `esp_http_server` added; HTTP train blocked by SoftAP association failure → locus is Arduino SoftAP/netif bring-up, not WebServer

**What was added (reversible, `CFG_C2_MIN_REPRO` / `T-Dongle-C5-c2min` only):**
- Raw `esp_http_server` on **:81**, route `GET /raw` → `raw-ok\n`, started from `C2Server::start()` alongside Arduino `WebServer` on **:80** (`/api/status` etc.).
- SoftAP knobs under the same flag: `max_connection=4`, `WIFI_PS_NONE`, force `WIFI_BAND_MODE_2G_ONLY` + `WIFI_BW_HT20` + `pmf_cfg.required/capable=false` after `WiFi.softAP()` (boot log: `[2g=ok ch=ok ht20=ok pmf_off=ok]`; Alfa scan confirms HT20 / no secondary).
- (Tried and reverted) a full raw-`esp_wifi` SoftAP bring-up inside the Arduino sketch — got SoftAP beacon but `WiFi.softAPIP()`=`0.0.0.0` and still no association; restored `WiFi.softAP` path.

**HTTP A/B result: could not be measured — SoftAP association itself fails**

After every clean SoftAP boot (`OK — 192.168.4.1`, both HTTP servers up), win11+Hama repeatedly reports `netsh` "Connection request was completed successfully" then **State=disconnected**. WLAN AutoConfig: 8000 → 11000 (assoc started) → **8002 (failed)**. C5 never logs `station connected`. **TXSTATS during attempts: AP(if1) ok=0 fail=0** (no downlink frames leave the MAC — same signature as the HTTP wedge, now hitting the 4-way / EAPOL path).

Alfa (rtw88, from arch-pentest) also fails against this SoftAP (`CTRL-EVENT-DISCONNECTED reason=1` after auth/assoc try).

**Control (same session, same Hama, same host):** flashed `/home/op/scratch/c5_softap_min` → win11 associated in one try (`State=connected`, Signal 100%, DHCP `192.168.4.2`). Then reflashed `T-Dongle-C5-c2min` → association fails again. Hama client is fine; Arduino SoftAP path is not.

**SoftAP / netif diffs vs working minimal (prime suspects):**
1. **Bring-up path:** lily uses Arduino `WiFi.softAP()` → `APClass::begin/create` → `WiFi.enableAP` / `esp_wifi_set_mode(current|AP)` + arduino-esp32 netif/event glue (`WiFiGeneric` creates default AP netif, optional STA netif objects, LR toggles). Minimal uses pure `esp_netif_create_default_wifi_ap()` + `esp_wifi_set_mode(WIFI_MODE_AP)` + `WIFI_INIT_CONFIG_DEFAULT()` with no Arduino WiFi layer.
2. **Band mode:** on C5, Arduino default appears to leave `WIFI_BAND_MODE_AUTO` (2.4+5); `esp_wifi_set_bandwidth(HT20)` **fails** until `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)` is forced. Minimal never touches band mode and still associates. Forcing 2G_ONLY+HT20 on lily **did not** restore association.
3. **RSN:** lily beacon still advertises **MFP-capable** even after `pmf_cfg.capable=false` via `esp_wifi_set_config` (driver may force it on C5). Minimal sets `pmf_cfg.required=false` only.
4. **Earlier same day:** stripped Arduino SoftAP *did* occasionally associate (DHCP `.50`) then HTTP wedged with TXSTATS AP ok=0 — same below-lwIP downlink failure, just later than EAPOL. Today's consistent assoc failure is the wedge firing at the 4-way.

**Interpretation against the planned discriminator:**
- Planned: `/raw` works & `/api/status` wedges → blame Arduino WebServer; both wedge → blame SoftAP/netif.
- Actual: **neither endpoint reachable because SoftAP never completes association** (TXSTATS AP=0 through the handshake). That is the "both wedge" case taken one layer earlier — **blame `WiFi.softAP` / arduino-esp32 SoftAP+netif bring-up**, not the Arduino `WebServer`/`NetworkClient` HTTP layer (which never gets a TCP socket).

**Next cheapest step:** replace Arduino SoftAP bring-up in the default C5 build with the minimal's raw `esp_wifi` SoftAP (proper DHCP/IP via `esp_netif_create_default_wifi_ap`, keep Arduino `WebServer` on top only if association then works), or bisect arduino-esp32 `WiFiGeneric`/`APClass` init (band mode AUTO, dual netif, MFP, wifi_init_config overrides for dynamic TX buffers when `useStaticBuffers()` is false).

**Device state:** C5 runs `T-Dongle-C5-c2min` (stripped app + raw `:81 /raw` + Arduino SoftAP with 2G/HT20/PMF knobs). SoftAP beacons `Lily` @ ch6 / 192.168.4.1; win11 currently **not** associated.
- Restore full lily: `cd firmware && pio run -e T-Dongle-C5 -t upload`
- Restore known-good minimal: `cd /home/op/scratch/c5_softap_min && pio run -t upload`
No git commit.


### Update 2026-10-30 (cont.) — replace Arduino SoftAP with minimal-parity raw SoftAP: IP real, association still fails

**Goal (authorized):** cheapest next step after A/B — drop Arduino WiFi.softAP() under CFG_C2_MIN_REPRO and bring SoftAP up with the same raw IDF sequence as working c5_softap_min (esp_netif_init → esp_event_loop_create_default → esp_netif_create_default_wifi_ap → WIFI_INIT_CONFIG_DEFAULT → esp_wifi_init → WIFI_MODE_AP + config SSID/ch6/WPA2/pmf.required=false → esp_wifi_start). Keep raw esp_http_server on port 81 (/raw) and Arduino WebServer on port 80 for HTTP A/B only if association works.

**Code change (firmware/src/c2/web_server.cpp, CFG_C2_MIN_REPRO path):**
- Never call WiFi.softAP / enableAP / Arduino mode helpers (those go through WiFiGeneric::espWiFiStart(), which on SOC_WIFI_SUPPORT_5G forces WIFI_BAND_MODE_AUTO and creates a default STA netif alongside AP).
- Read SoftAP IP via esp_netif_get_ip_info(_rawApNetif) (not WiFi.softAPIP()).
- Station count / AP events via raw WIFI_EVENT_AP_* handler; skip WiFi.onEvent.
- tx_done_cb, Arduino WebServer routes, and raw esp_http_server port 81 /raw still start after SoftAP.

**Boot (success criterion 1 — met):**
Boot log showed: wifi_init=ok, mode_ap=ok, cfg=ok, start=ok, raw_min_parity OK — 192.168.4.1; tx_done_cb register ok; HTTP server started; raw esp_http_server started on :81 (/raw); Boot complete.
SoftAP beacons Lily @ ch6 / HT20 / WPA2; win11 scan sees it at ~100% signal. IP is real 192.168.4.1 (prior incomplete raw attempt that reported 0.0.0.0 was wrong; this sequence matches minimal).

**Association (success criterion 2 — failed):**
win11 + Hama (RTL8188EU) after Location/lfsvc OK: 8x netsh wlan connect name=Lily → "Connection request was completed successfully" then State=associating → disconnected (never connected; no DHCP .x). During attempts TXSTATS AP(if1) ok=0 fail=0 (no downlink frames leave the MAC — same 4-way signature as the prior Arduino SoftAP assoc failure). No HTTP A/B (criterion 3) — skipped because SoftAP association never completed.

**Related env note (c2min-stock):** same raw SoftAP sequence built as T-Dongle-C5-c2min-stock (custom_sdkconfig reverts lily DYNAMIC_TX / AMPDU-off to IDF static TX defaults) Guru Meditation LoadProhibited mid SoftAP start after [cfg=ok] (before IP print). Stock Arduino+raw SoftAP init is not a usable A/B on this board right now; left on working T-Dongle-C5-c2min (DYNAMIC_TX + AMPDU_TX=n) which boots clean with 192.168.4.1.

**Remaining material diffs vs working c5_softap_min (after this solid attempt — stop, do not thrash):**
1. Runtime environment: Arduino initArduino / C++ runtime / HAL / Serial-JTAG already ran before SoftAP; minimal is bare app_main + nvs_flash_init then SoftAP.
2. Linked Arduino WiFi/Network: even when SoftAP start skips WiFi.softAP, the sketch still links arduino-esp32 WiFi/Network (static ctors, possible netif/event side effects) vs pure IDF.
3. sdkconfig: lily c2min forces CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y + CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n (+ LWIP_STATS / WDT knobs). Minimal uses stock IDF SoftAP buffers/AMPDU. Stock-sdkconfig lily build panics on esp_wifi_start in this raw path (see above) — cannot yet prove sdkconfig alone.
4. Post-start hooks still present: esp_wifi_set_tx_done_cb(txDoneCb), Arduino WebServer.begin() on port 80, raw httpd on port 81 — minimal only runs stock esp_http_server on port 80.
5. PSRAM / MSPI: lily boot still logs MSPI Timing Failed to allocate dummy cacheline for PSRAM memory barrier; minimal image does not show this path.
6. Auth/token / route table: lily still generates C2 auth token and registers full REST route table on WebServer (idle until requests).

**Conclusion:** Correct minimal-parity SoftAP bring-up inside the Arduino sketch fixes the 0.0.0.0 IP bug but does not restore association. SoftAP radio + DHCP netif look healthy from the AP side; AP→STA downlink through the 4-way still dead (TXSTATS AP=0). Locus after this attempt is no longer "Arduino WiFi.softAP call sequence alone" — it is whatever differs in the Arduino-hosted process (linked WiFi/Network, sdkconfig that panics when stock, tx_done_cb / dual HTTP stacks, or earlier Arduino init) vs bare c5_softap_min. #9 stays OPEN. Per plan: one solid attempt documented; stop.

**Device image state:** C5 runs T-Dongle-C5-c2min with raw minimal-parity SoftAP + port 81 /raw + Arduino WebServer port 80. SoftAP beacons Lily @ ch6 / 192.168.4.1; win11 not associated.
- Restore known-good minimal (assoc works): cd /home/op/scratch/c5_softap_min && pio run -t upload
- Restore full lily: cd firmware && pio run -e T-Dongle-C5 -t upload
- Rebuild this debug image: cd firmware && pio run -e T-Dongle-C5-c2min -t upload
No git commit/push. Secrets/auth tokens redacted from this note.
