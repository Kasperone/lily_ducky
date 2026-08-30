# Claude Code — start here

This repo's agent instructions live in one place, shared by every agent that
works here (Claude Code, Hermes, or anyone else) — not duplicated per-tool.

**Read [`AGENTS.md`](AGENTS.md) first.** It has current build status, the
architecture summary, and the gotchas that cost real debugging time if
skipped. Its own top section then points to
[`docs/knowledge-base/agent-playbook.md`](docs/knowledge-base/agent-playbook.md)
and [`docs/knowledge-base/open-questions.md`](docs/knowledge-base/open-questions.md)
— read those before touching any hardware-facing code
(`firmware/src/hal/`, `firmware/src/config.h`, anything touching JTAG/pin-mux
registers or SPI bus init). Skipping straight to a fix without reading
`open-questions.md` is how this repo's two "confirmed dead LED" claims got
made and then retracted — don't repeat that.

Nothing below is a substitute for those files; this is only the pointer so a
fresh session doesn't start from zero.

## Fast orientation

- **What this is**: a USB Rubber Ducky / BadUSB learning platform on the
  LILYGO T-Dongle-C5 (ESP32-C5, primary — hardware on hand, no USB-OTG, WiFi
  C2 lab node) and T-Dongle-S3 (ESP32-S3, full HID keyboard, compile-only, no
  hardware acquired).
- **Build**: `cd firmware && pio run -e T-Dongle-C5` (or `-e T-Dongle-S3`).
  `pio run` must pass for whichever env you touch before you commit
  (`CONTRIBUTING.md`).
- **Hardware access**: if a T-Dongle-C5 is physically attached this session,
  it shows up as `/dev/ttyACM0` (`lsusb`: `303a:1001`, Espressif USB
  JTAG/serial). `scripts/probe_reset.py` and `scripts/serial_monitor.py`
  reset/capture over that port without a physical replug. **Never trigger a
  visual hardware test (LED, LCD) without asking the user to confirm they're
  watching first** — there is no way to observe the board's physical state
  otherwise.
- **Docs map**: `AGENTS.md` (compressed facts + status) →
  `docs/knowledge-base/README.md` (index) → `agent-playbook.md` (how to work
  here) / `open-questions.md` (disputed or unverified claims, read before
  trusting anything hardware-related) / per-subsystem files (sourced
  evidence) / `sources.md` (bibliography). `CONTRIBUTING.md` has git/branch/
  commit conventions; `docs/programming-style.md` has code style.
- **Known-broken example not to repeat**: two prior local-model sessions on
  this codebase produced plausible-sounding but wrong hardware claims baked
  into commits and comments — see `docs/knowledge-base/README.md`'s "Why
  this exists" for the pattern (a real source, summarized backwards) so it's
  recognizable if it starts happening again.
