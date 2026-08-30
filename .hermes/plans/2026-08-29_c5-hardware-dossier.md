# T-Dongle-C5 Hardware Dossier — reference for a fresh session

> Written 2026-08-29, updated 2026-08-30 after E1/E4 bisection session, and
> again 2026-08-30 (separate, later Claude Code session — see "§0 Update"
> just below) after a hardware-SPI test superseded the backlight theory in
> this line. The single unresolved problem is the **RGB LED staying solid
> white regardless of colour/config sent** — no longer believed related to
> the LCD backlight specifically (see §0). Everything else (flash, boot, SD,
> LCD, WiFi C2, button, interpreter) is verified working. Read this file
> FIRST; it supersedes scattered notes, but for the LED specifically,
> `docs/knowledge-base/open-questions.md` #1 (tracked in git, always current)
> is the higher-authority source — this dossier is the detailed backing
> evidence behind it, not a replacement for it. Companion files:
> `.hermes/plans/2026-08-28_093918-tdongle-c5-bringup.md` (phase history),
> skill `esp32-arduino-build` (gotchas).

## §0.0 RESOLVED — 2026-08-30, third session (Claude Code): LED was a PIN BUG

**The LED works. It was never dead.** The whole "solid white / hardware fault"
saga was a wrong pin assignment. The APA102 is on **GPIO2 (data) / GPIO6
(clock)** — the LCD/SD SPI bus — **not GPIO4/5** (the JTAG MTCK/MTDO pads) as
this dossier's §3, the vendor `pin_config.h`, and every session assumed.
Driving 4/5 never reached the LED; the "white/amber/cyan" was the LED sitting
on stale data left on the shared bus (which is why it looked "intermittent").

- **Attached board this session: MAC `38:44:BE:BC:F9:3C`** (USB iSerial) — a
  DIFFERENT physical unit than the `38:44:BE:BC:FA:C4` recorded in §1 (same
  batch). The board-identity gap flagged in §0's last bullet is now closed.
  The pin bug is a board-design fact, not per-unit, so it applies regardless.
- **Proven:** `firmware/src/diag/led_pinmap_diag.cpp` (envs
  `T-Dongle-C5-pinmap-A` = 2/6, `-pinmap-B` = 5/4) — colours cycle on 2/6,
  frozen on 5/4, user watching live.
- **Fixed & verified in main firmware:** `config.h` PIN_LED_DATA=2/CLOCK=6 +
  `CFG_LED_SHARED_SPI`; `hal.cpp` drives the LED over the shared `SPI` bus +
  `ledRefresh()` after LCD/SD traffic; `display.cpp` refreshes LED after each
  paint; LCD backlight restored (`CFG_LCD_BL_LEVEL` 180 — the "washout" theory
  was this same pin bug). Steady teal LED **and** LCD dashboard both confirmed
  working simultaneously; boot (SD/WiFi C2) unaffected.
- **Source that cracked it:** `github.com/zombodotcom/T-Dongle-C5` (community
  examples repo documenting the 2/6 shared-bus wiring). Conflicts with the
  vendor `pin_config.h`; the hardware A/B test settled it in the community
  repo's favour.
- Everything below (§0 onward) is the pre-resolution record. §3's
  `usb_jtag_bridge_en` discussion and §4/§5's LED hypotheses are **superseded**
  — the register was never relevant (the LED isn't on the JTAG pins). Canonical
  write-up: `docs/knowledge-base/open-questions.md` #1.

## §0. Update — 2026-08-30, second session (Claude Code, not the session that
wrote the rest of this file)

Picked up from this file's own "Next session priority: prove hardware vs.
software" (§4). Did NOT re-flash the factory binary as planned — instead
went one level more rigorous, per `docs/knowledge-base/open-questions.md`
#1's own lesson about trusting single clean-looking results. Full detail and
citations live in that file's 2026-08-30 addendum; summary:

- Root-caused (not just worked around) the `SPIClass(HSPI)` dead end this
  file doesn't mention (it postdates this file): `HSPI` is an invalid SPI
  bus index on the C5 (only one GP-SPI peripheral; `HSPI`'s value here means
  something else on chips with two). `SPIClass(FSPI)` is correct.
- **§3's `usb_jtag_bridge_en` explanation was backwards.** The full register
  text (not just the summary in §3) says setting the bit makes MTDO —
  GPIO5, the LED's DATA pin — an **input**, not a released output. Verified:
  the vendor never sets this register at all (§3 already noted this but
  read it as incidental; it's load-bearing). `CFG_RELEASE_JTAG_LED_PINS` is
  now 0 in the actual firmware (commit on `fix/c5-console-boot-race`),
  verified on hardware not to regress SD/LCD/WiFi C2 boot.
- Built `firmware/src/diag/led_spi_diag.cpp` (tracked, reusable —
  `pio run -e T-Dongle-C5-leddiag`), a real hardware-SPI driver, and ran it
  with `bridge_en` in both states plus a raw all-zero-data-stream
  falsification test. **Result: solid white throughout every test**,
  including the all-zero stream a healthy LED can't stay lit through.
- This is NOT the same as §4's "confirmed hardware fault" (which this same
  project already retracted once, per this file's own §4). It's stronger
  evidence than §4 had — every confound §4 found (bit-bang jitter, the SPI
  bug, bridge_en direction, framing) is eliminated here — but still not
  being called settled; see `open-questions.md` #1 for exactly why, and
  what would actually close it out (electrical measurement, or re-running
  the vendor's native ESP-IDF example on this exact unit — no ESP-IDF
  toolchain was installed in that session's environment to attempt the
  latter).
- **Board identity gap, flagged not closed:** this session never cross-
  checked the boot-log MAC address against the `38:44:BE:BC:FA:C4` recorded
  in §1/the companion bring-up file below, so it's unconfirmed whether the
  unit tested 2026-08-30 (second session) is the same physical board as the
  one this file's earlier sections describe. Whoever picks this up next:
  check the boot log's MAC against that value first, before assuming either
  way.

Repo: `~/lily_ducky` (path redacted for sharing — this file is tracked in
git as of 2026-08-30, see `docs/knowledge-base/open-questions.md` #6).
Branch: `fix/c5-console-boot-race` — **merged to `main`** 2026-08-30 via PR #4
(LED pin-bug fix + knowledge-base updates) and PR #6 (LED follow-up docs). The
LED question is resolved; see §0.0 at the top of this file.
Device currently runs **LilyDucky v0.2** with D2-recipe LED settings
(BL=0, br=10, zeros EF, bridge_en=1 — flashed 2026-08-30, superseded same day,
see the 2026-08-30 update at the top of this file). Wi-Fi C2 token:
`<REDACTED — regenerates every boot, read the current one from
probe_reset.py/serial_monitor.py>`. Auth tokens in this file are never valid
by the time you read them; don't bother redacting future ones by hand if you
extend this file — just note it's ephemeral.

---

## 1. Environment & access procedures (MUST follow)

- Dongle = LILYGO T-Dongle-C5, USB-passthrough into THIS VM (libvirt hostdev,
  VID:PID 303a:1001, MAC-serial 38:44:BE:BC:FA:C4). `/dev/ttyACM0`, root:uucp.
- **NEVER physically replug** — replug breaks passthrough; re-attach from HOST:
  `virsh detach-device <vm-name> <hostdev>.xml --live` then
  `virsh attach-device <vm-name> <hostdev>.xml` (VM name and hostdev XML
  filename are local to whichever machine is running the passthrough —
  substitute the real ones for this clone).
- Agent shell sessions started before the uucp group add lack the group:
  wrap ALL device access in `newgrp uucp -c '<cmd>'` (or heredoc form).
- **Port exclusivity:** only ONE process may hold /dev/ttyACM0. Kill any
  background serial_monitor BEFORE `pio ... -t upload` or esptool, else both
  corrupt ("device reports readiness to read but returned no data").
- Reset WITHOUT replug: `newgrp uucp -c 'python3 scripts/probe_reset.py'`
  (RTS pulse with DTR cleared; DTR-high + RTS pulse = ROM download mode,
  chip goes silent + drops off bus — scripts already handle this).
- Serial capture: `newgrp uucp -c 'python3 scripts/serial_monitor.py /dev/ttyACM0 /tmp/x.log'`
  (background; termios-only, no pyserial).
- pio: `export PATH=$HOME/.local/bin:$PATH` (PlatformIO 6.1.19 via uv).
  esptool with deps: `~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py`.
- GitHub push: SSH key must be unlocked in the desktop keyring agent first
  (user did this once on 2026-08-29; if push fails with
  "sign_and_send_pubkey ... communication with agent failed", ask user to
  unlock, or use the dedicated agent env at /tmp/ssh-agent-lily.env after
  `ssh-add` there).
- This VM has **NO WiFi adapter** — C2 API tests need a host/laptop joined to
  the LilyC2 AP; `scripts/c2_api_test.sh` is ready to run there.

## 2. Verified working (do not re-debug)

- Build both envs green; flash OK ("Hash of data verified"); chip = ESP32-C5
  rev v1.0, 240 MHz, WiFi6/BT5/802.15.4.
- Full boot log on every boot: banner, HAL OK (+ no-OTG note), HID N/A,
  `[SD] OK: SD (30436.5 MB)` (32 GB card), SoftAP 192.168.4.1, token printed
  (regenerates EVERY boot — read serial for current one), HTTP server up,
  "No payload found on SD" (card has no /payloads/payload.dd), Boot complete.
- Console boot race FIXED (bf22ea8): setup() waits ≤8 s for CDC connect
  before printing (ARDUINO_USB_MODE builds only). Headless boots stay finite.
- LCD: panel + text + backlight all work; dashboard shows correct SSID/IP/
  token/Clients/state (user-verified twice). Rotation 3 = factory orientation.
- **Button WORKS** (GPIO28, active-low): serial shows `[MAIN] Button:
  started payload` on press; diag showed raw BTN=0 edges. Its only feedback
  is the (currently invisible) LED + a subtle LCD dot — hence "does nothing".
- WiFi C2 HTTP server answers (token line proves server ran; full API suite
  still UNTESTED — needs a WiFi client, see §8).
- Benign ROM warning on every boot: "MSPI Timing: Failed to allocate dummy
  cacheline for PSRAM memory barrier" — ignore.

## 3. Board facts (schematic V1.1 + factory firmware, ground truth)

| Function | GPIO | Note |
|---|---|---|
| APA102 DI / CI | 5 / 4 | nets LED-DI/LED-CI = chip pads MTDI/MTCK (JTAG!) |
| BOOT button | 28 | active-low, INPUT_PULLUP |
| LCD MOSI/SCK/MISO | 2 / 6 / 7 | shared with SD (SPI mode) |
| LCD CS / DC / RST | 10 / 3 / 1 | |
| LCD BL | 0 | **ACTIVE-LOW** (factory: digitalWrite(0)=ON); Q1 SI2301 P-MOS |
| SD CS | 23 | |

- **JTAG pad mux (the big one):** `USB_SERIAL_JTAG.conf0.usb_jtag_bridge_en`
  (bit 15, default 0). At 0 the USB-JTAG controller drives MTCK/MTDI:
  TDI idle-high + TCK edges = an APA102 white-bit stream → LED hard white,
  GPIO writes lost. At 1 the pads route through the GPIO matrix and the LED
  obeys. Fix in firmware: `CFG_RELEASE_JTAG_LED_PINS` (config.h) set in
  Hal::init() (40d37b8). CDC console/flashing/ROM boot unaffected (verified).
- APA102 frame rules learned on hardware: 5-bit brightness in the 111bbbbb
  header must be non-zero (0 = current sink off = dark); **end frame of ones
  breaks this LED** (diag v2 zeros → colours; diag v3 ones → no colours);
  use zero end-frame (Pololu library style).
- Factory firmware (Xinyuan-LilyGO/T-Dongle-C5) uses ARDUINO_USB_MODE=1 +
  CDC_ON_BOOT=1 (same as us) and its LED demo DID show colours on this class
  of hardware — consistent with bridge_en being the unlock.

## 4. The LED mystery — evidence table (chronological)

Legend: BL=backlight, br=APA102 brightness, EF=end frame, BE=bridge_en.

| # | FW | BL | br | EF | BE | User saw |
|---|----|----|----|----|----|----------|
| F2 | main+console-wait | ON(digital) | 0(!) | zeros | 0 | LCD black; LED white strong |
| F3 | +BL active-low, rot3, br10, EF ones | ON | 10 | ones | 0 | LCD correct; LED white strong; button "dead" |
| D1 | diag: no TFT, BL OFF | OFF | 10 | zeros | 0 | LED white; (serial: button events OK) |
| D2 | diag: BL OFF, phase A=BE+pins4/5 vs B=pins2/6 | OFF | 10 | zeros | 1(A) | **colours: red/green/blue/off loop** (phase A); phase B dark |
| F4 | real fw + BE | ON | 10 | ones | 1 | LED white; LCD correct |
| F5 | + br31 | ON | 31 | ones | 1 | LED white; serial shows button events |
| D3 | diag + TFT stage2, BL OFF→ON windows | OFF→ON | 31 | ones | 1 | "nothing happen" (no visible blink even BL-off) |
| F6 (=a56b3d8, ON DEVICE NOW) | + EF zeros + PWM BL 80/255 | ON ~31% | 31 | zeros | 1 | LED white; LCD correct; button "dead" |
|| F7 | E1: real fw, BL=0, br10, zeros EF | OFF | 10 | zeros | 1 | **LED white**; LCD dark (BL off) |
|| F8 | E4 bisect 1: no TFT, no WiFi, SD only | OFF | 10 | zeros | 1 | LED white |
|| F9 | E4 bisect 2: no TFT, no WiFi, HAL init + colour cycle after Serial | OFF | 10 | zeros | 1 | LED white ("nothing happen" during 5s colour cycle) |
|| F10 | E4 litmus: bare EXACT D2 recipe (no TFT, no SD, no WiFi) | OFF | 10 | zeros | 1 | LED white — *D2 recipe no longer works* |
|| F11 | E4 litmus v2: bridge_en set AFTER Serial.begin | OFF | 10 | zeros | 1 | LED white |
|| D4 | /tmp/hwdiag v4: pure D2 recipe, no TFT_eSPI linked at all | OFF | 10 | zeros | 1 | "white and black screen" — same D2 recipe, different result |
|| FACTORY | LilyGO V1.4 factory flash (0x0), Pololu lib, no bridge_en | ON ~100% | 10 | ones(count) | 0 | serial: "LED Task" running; HW status TBD |

Read-off:
- BE=0 ⇒ white regardless (JTAG owns pads). [F2,F3,D1 vs D2]
- BE=1 + BL OFF + EF zeros ⇒ colours. [D2] — **but this recipe NO LONGER WORKS** (see F7-F11, D4)
- EF ones ⇒ no colours even BE=1, BL off. [D3 stage1 vs D2]
- Every build with BL ON (any level) ⇒ user reports white. [F3..F6]

**CRITICAL REGRESSION (2026-08-30):** The exact D2 recipe (BE=1, zeros EF,
br=10, BL OFF, no TFT_eSPI) that showed red/green/blue on 2026-08-29 now
produces solid white on 2026-08-30. We bisected exhaustively:
- Skipping TFT init, WiFi, and SD — still white.
- Running the D2 recipe verbatim in our build and in a standalone /tmp/hwdiag
  — still white. Even raw GPIO DATA=HIGH for 3s produces no visible change.
- Setting bridge_en AFTER Serial.begin() (in case Serial clears it) — still
  white.
- Flashed LilyGO factory firmware V1.4 (which does NOT set bridge_en and uses
  Pololu APA102 lib with endFrame(count)) — serial shows "LED Task" running
  but hardware status TBD.

Two possibilities:
1. **Hardware damage between sessions** — the LED chip itself may have failed
   or a solder joint cracked. The APA102 is a tiny 2020 package; a single
   broken connection (DI, CI, VDD, GND) could leave it stuck white.
2. **Subtle build/env difference** — the diag build environment changed
   between the D2 and D4 sessions (PlatformIO cache, toolchain version?).
   Unlikely given the clean rebuild from source.

**Next session priority: prove hardware vs. software.** Flash the factory
binary from a cold chip, observe LED. If still white with the factory
firmware, the LED is likely dead — accept it and move to §8 (C2 API, docs,
merge). BLE HID (§3 roadmap) or a replacement C5 board would be the next
paths for LED functionality.

## 5. Ranked hypotheses + next experiments (do in order, ONE variable at a time)

Device now runs LilyGO factory firmware V1.4. Start there in the next session.

- **P0 — hardware vs. software:** observe the factory firmware LED.
  If white → LED hardware is likely dead/stuck. Accept, move to §8.
  If colours show → the factory firmware has a *different* APA102 init
  sequence we haven't replicated — diff the Pololu library against our
  bit-bang code.

── ALL BELOW THIS LINE ARE FROM THE PRE-REGRESSION SESSION ──

- E1 (washout/overexposure) — **FAILED 2026-08-30.** BL=0+br10 on real fw still
  showed white. Backlight bleed is NOT the cause.
- E2 (find usable backlight level) — DEFERRED until LED works.
- E3 (perception cross-check) — DEFERRED.
- E4 (full-fw bisect) — **EXHAUSTED 2026-08-30.** Stripped to bare HAL+LED,
  still white. The regression is real and spans both our build system and
  /tmp/hwdiag.

## 6. Button & LCD — closed topics

- Button: works; no code change ever needed. "Does nothing" = invisible
  feedback (LED washed out; LCD dot unchanged on no-payload runs).
- LCD: only open item is the backlight level chosen in E2.

## 7. Commit map (branch fix/c5-console-boot-race, all pushed)

| sha | what |
|---|---|
| bf22ea8 | wait for CDC connect before boot log (token was lost) |
| 979042f | scripts: serial_monitor.py, probe_reset.py (was pulse_reset) |
| 8d6e669 | scripts/c2_api_test.sh (C2 REST suite) |
| d4b9d14 | backlight active-LOW + rotation 3 + STOPPED one-shot blink guard |
| 95f06c7 | APA102 brightness 10 (was 0) + ones end frame (later superseded) |
| ffc3d6c | payloads/cross_os_detect.dd: add DETECT_OS line |
| 40d37b8 | usb_jtag_bridge_en release (CFG_RELEASE_JTAG_LED_PINS) |
| a56b3d8 | EXPERIMENT (on device now): EF zeros + br31 + PWM BL 80 |

Diag firmwares live in /tmp/hwdiag (v3 current: BL windows + TFT stage2).
/tmp/tdongle_c5_v1.1.pdf + /tmp/tdongle_sch-1.png = schematic.

## 8. Remaining project work (after LED settled)

1. C2 API suite from a WiFi-joined machine:
   `bash scripts/c2_api_test.sh 192.168.4.1 <token> payloads/hello.dd`
   (token from serial; covers status/PUT/GET/401/run/stop/DETECT_OS
   responsiveness). VM itself cannot join (no WiFi adapter).
2. Auto-fire-on-boot check after a payload is on the card (the suite PUTs
   payload.dd; then reset and watch `[MAIN] Loading payload.dd ... no-op`).
3. DETECT_OS payload serial check (`[INT] $_OS=0` on C5).
4. Docs (AGENTS.md status/gotchas), open PR (web UI), merge, tag v0.3.0.
5. Known-deferred: execDelay blocks C2 during long DELAYs; S3 USB_VID/PID
   spoof overridden by core variant header; S3 has no hardware on hand.

## 9. Tooling gotchas (quick list)

- `pio run` builds only default_envs (T-Dongle-C5); pass -e for both.
- LSP/clang "Arduino.h not found" noise is expected; PlatformIO is truth.
- terminal tool rejects `&` and heredoc+`&`; use scripts or newgrp -c.
- `newgrp uucp -c` heredoc form: `newgrp uucp <<'EOF' ... EOF`.
- probe_reset.py also captures 12 s of boot output after the pulse.
- After killing a monitor, the tty may need ~1 s before esptool binds.
