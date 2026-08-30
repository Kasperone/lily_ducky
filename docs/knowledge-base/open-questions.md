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

## 1. LED — hardware fault is now the best-supported explanation, not yet fully settled; two prior "confirmed dead" claims were retracted — read the whole arc, including the 2026-08-30 addendum, before trusting any summary of it

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
file; the WiFi password appearing in example commands (`quackquack`) is
already the project's intentionally-public lab default.

**Standing discipline, unchanged by this resolution:** promoting durable
findings out of a plan file into this knowledge base (or `AGENTS.md`, or a
commit message) before the plan file is superseded is still the rule per
`agent-playbook.md` — tracking the plan files themselves doesn't replace
that, it's a safety net under it. And: **any future edit to these two files
must be checked for tokens/paths/secrets before committing**, the same way
this pass was — tracking them means anything added from here on ships to
GitHub, not just to this one machine.
