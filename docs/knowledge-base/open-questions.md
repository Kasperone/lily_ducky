# Open questions — read this before trusting a hardware claim in this repo

Things found while building this knowledge base (2026-08-30) where an existing
commit message, code comment, or `AGENTS.md`/`README.md` line either didn't hold
up against a primary source, or couldn't be told apart from an unverified guess.
#3 and #5 were fixed directly (docs-only, low risk). #4 was fixed by rewriting
the status banners once real hardware evidence was found. **#1 remains
genuinely unresolved** — an earlier pass in this same session concluded "LED
confirmed dead hardware," and that conclusion turned out to be premature once
a second unit was tested; see #1 for the full arc, including the mistake, so
it isn't repeated. #6 remains open as a process question for the repo owner.
Each entry has the evidence so whoever (Hermes, Claude, or the repo owner)
picks this up next doesn't have to redo the research.

## 1. LED — STILL UNRESOLVED as of 2026-08-30 end of session; earlier "confirmed dead hardware" conclusion was premature — read the whole arc before trusting any summary of it

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

**Recommended next steps, in order of cost:**
1. Write and test a genuinely hardware-timed driver — ESP-IDF's `led_strip`
   component (RMT-backed) is the standard, well-tested way to drive
   addressable LEDs on ESP32 and was never actually tried here (only assumed
   to be what the working vendor example used). If that's reliable across many
   repeated runs on board 2, port `sendAPA102()` in `hal.cpp` to use it instead
   of hand-rolled bit-banging, on both boards.
2. If a hardware-timed driver is also intermittent, suspect a genuine signal
   integrity issue (wire length/quality between MCU and LED, pull resistor
   values, decoupling) rather than firmware — that needs a logic analyzer or
   oscilloscope, out of scope for a coding session.
3. Only once a driver is shown reliable across *many* repeated on/off/color
   cycles (not one lucky run) should any board's LED be declared working or
   dead, and only then does the `bridge_en` direction question become
   testable again.
4. **Process lesson for next time, independent of the LED itself:** when a
   test result seems to cleanly confirm a hypothesis, spend one extra cheap
   test trying to break the result before writing it down as fact (step 9
   above is the template) — especially when the test itself introduces new,
   untested code (a new driver, a new library call) rather than only changing
   the one variable under investigation.

**What this means for the register-direction question (item originally under
this heading):** still genuinely unresolved in the abstract — the D2 A/B result
and the datasheet reading still don't reconcile — but it no longer matters for
*this unit*: the LED can't be used as a signal either way while it's stuck. If
a second T-Dongle-C5 is ever acquired, the D2 A/B test would be worth redoing
there specifically to settle the register question independently of this
unit's condition. Not a priority otherwise.

**What this means for the project:** the status LED on this specific board can
be treated as **dead**. Firmware LED logic (`Hal::ledSet`, `ledBlink`,
`sendAPA102`) is implemented correctly per protocol and per the vendor's own
recipe (brightness 10, zero end frame) — leave it as-is; it isn't the problem
and would presumably work on a board with a healthy LED. Options going forward,
for whoever owns this hardware decision: accept the LED as non-functional and
rely on the LCD dashboard for status (it already shows the same information);
attempt a rework/reflow of the APA102 package if equipment is available; or
bodge an external APA102/LED to a spare GPIO. Not resolved here — hardware
repair is outside what a coding session can do.

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

## 6. The most detailed hardware ground-truth lives in a gitignored, single-machine file

**Where:** `.hermes/plans/2026-08-29_c5-hardware-dossier.md` and
`.hermes/plans/2026-08-28_093918-tdongle-c5-bringup.md` — both under `.hermes/`,
which `.gitignore` excludes entirely ("Agent state (local to each clone)").

**Why this matters:** these two files are, by a wide margin, the best-sourced
hardware evidence in this entire project — a chronological table of every LED
experiment with exact settings and observed results, the exact DTR/RTS reset
gotcha, exact serial log lines, exact tokens and card sizes seen. Everything in
`open-questions.md` #1 and the README/AGENTS.md status updates from 2026-08-30
is downstream of these two files. But because `.hermes/` is gitignored, **this
evidence does not travel with the repo** — a fresh clone, a different machine, a
different agent session with no access to this specific filesystem, or simply
this VM being rebuilt, all lose it permanently. The commit messages on
`fix/c5-console-boot-race` capture a compressed version of the same findings and
*do* travel with the repo, but the dossier has strictly more detail (the full
chronological evidence table in particular has no equivalent anywhere in
tracked files).

**Recommendation:** decide deliberately whether `.hermes/plans/*.md` should stay
gitignored-only. If the answer is "yes, agent scratch state shouldn't be
tracked" (a reasonable default), then the discipline needs to be: **before a
plan file like this is abandoned or superseded, promote its durable findings
into a tracked doc** (this knowledge base, `AGENTS.md`, or a commit message) —
don't let "write it to the plan file" substitute for "write it somewhere that
survives." This pass promoted the 2026-08-30 snapshot; nothing currently
guarantees the *next* bring-up session's findings will be promoted the same way
before its own plan file is deleted or overwritten.
