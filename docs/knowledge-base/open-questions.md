# Open questions — read this before trusting a hardware claim in this repo

Things found while building this knowledge base (2026-08-30) where an existing
commit message, code comment, or `AGENTS.md`/`README.md` line either didn't hold
up against a primary source, or couldn't be told apart from an unverified guess.
#3 and #5 were fixed directly (docs-only, low risk). #4 was fixed by rewriting
the status banners once real hardware evidence was found. #1 was fully resolved
this session with live hardware access (see below — the LED is a confirmed
hardware fault, not a firmware bug). #6 remains open as a process question for
the repo owner. Each entry has the evidence so whoever (Hermes, Claude, or the
repo owner) picks this up next doesn't have to redo the research.

## 1. LED — RESOLVED 2026-08-30: hardware fault, confirmed, not a firmware bug

**Where:** `firmware/src/hal/hal.cpp` (`Hal::init()`, `sendAPA102()`),
`firmware/src/config.h` (`CFG_RELEASE_JTAG_LED_PINS`, `HAL_APA102_BRIGHTNESS`).

This entry went through three states in one day. Recording the full arc because
the *method* that settled it — a static single-frame diagnostic, isolating the
signal from every other variable — is the reusable lesson, not just the
conclusion.

1. **Originally (documentation-only pass):** argued `usb_jtag_bridge_en = 1`
   was likely backwards, from reading the register header and the Espressif
   JTAG-configuration guide, plus LilyGO's factory firmware never touching that
   register.
2. **Then, reading the local `.hermes/` dossier:** found a real on-device A/B
   test (session "D2", 2026-08-29) where bridge_en=1 correlated with a working
   colour cycle — genuine empirical evidence complicating the theoretical
   reading, unresolved either way.
3. **Then, live hardware access this session, definitively resolved:** with the
   actual T-Dongle-C5 attached (`/dev/ttyACM0`, `303a:1001`), ran two decisive
   tests instead of theorizing further:
   - Reflashed **LilyGO's stock factory firmware** (`/tmp/factory.bin`, no
     `bridge_en` write, Pololu library, continuous RGB cycle). User-observed:
     dim purple/pink blend with occasional white flashing — never a clean,
     distinct colour.
   - Flashed a **from-scratch diagnostic** (`/tmp/hwdiag`, `bridge_en=1`, our
     own zero-end-frame recipe) that sends exactly one static frame per colour
     — RED held 3s, then GREEN held 3s, then BLUE, then OFF, then AMBER —
     nothing else running (no WiFi/LCD/SD/loop). This isolates the signal
     completely: if the LED receives each frame, the visible colour *must*
     change every 3 seconds, because the firmware genuinely never repeats a
     colour. User-observed, run twice: **no change at all** — "colors not
     changing... it looks like a dimmed light and all of them look like they're
     lit at once," matching the same purple/pink blend as the factory-firmware
     run. A gentle press near the LED/PCB produced no change either (rules out
     a simple flex-sensitive cracked joint specifically, though not a
     non-flex-sensitive break).
   - **Conclusion:** two structurally unrelated firmware images (vendor
     library, from-scratch bit-bang) sending genuinely different, verified
     (via serial log) commands over several seconds each, produced the exact
     same frozen, blended, unresponsive output both times. Software cannot
     cause that — every plausible firmware bug would still change *something*
     about the wrong output as the commanded values change. This is the
     signature of the LED (or its data/clock connection) being physically
     stuck: most likely a damaged APA102 die or a broken trace/solder joint
     that isn't pressure-sensitive. It is **not** the `usb_jtag_bridge_en`
     question, and it is **not** a bug in this project's `sendAPA102()`.
4. **Follow-up test, same session — ruled out a recoverable "stuck latch"
   state too:** the tests above only ever soft-reset the ESP32 (RTS pulse);
   USB VBUS stayed continuously applied throughout, so the APA102's own
   internal shift register/latch was never actually power-cycled. Repeated the
   static single-frame diagnostic a third time, this time across a genuine
   **physical unplug/replug** of the whole board (confirmed by the user — not
   just a VM/passthrough reconnect). If the fault were a corrupted internal
   latch state in the APA102 rather than permanent damage, a real power cycle
   should clear it. It didn't: user-observed **constant white, no colour
   changes at all** through the same RED→GREEN→BLUE→OFF→AMBER sequence,
   post-power-cycle. This rules out the one remaining "maybe it's a
   recoverable state, not permanent damage" hypothesis. Three independent
   conditions (vendor firmware, our diagnostic after a soft reset, our
   diagnostic after a hard power cycle) all produced frozen/unresponsive
   output — this is as conclusive as remote (no logic analyzer, no
   continuity meter) diagnosis gets.

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
