# Knowledge base — index

This directory is the sourced, checkable layer under `AGENTS.md`. `AGENTS.md` gives
the compressed facts an agent needs on every task; these files give the primary
sources behind each fact, the exact register/API/commit evidence, and — critically —
the claims that turned out to be **wrong or unverified** when checked against a
primary source. Read `open-questions.md` before trusting a hardware claim anywhere
else in this repo, including in `AGENTS.md` itself.

## Why this exists

This repo does hardware bring-up on boards no LLM has training data about in detail
(T-Dongle-C5 shipped mid-2026). Two prior agent sessions on this codebase (local
models, not Claude) produced plausible-sounding but unverified or backwards hardware
claims baked into commit messages and code comments — see `open-questions.md` for the
specific cases found on 2026-08-30. The failure mode was never "the model didn't try
to cite a source" — it was citing a real source and then summarizing it wrong, or
matching a register name to the wrong mental model. Small/local models are especially
prone to this because verifying a claim (fetching the actual register header, reading
every comment in a GitHub issue thread, diffing against vendor firmware) costs more
tokens than asserting it, and nothing forces the extra step.

**The fix this knowledge base applies:** every non-trivial hardware or toolchain claim
below is tagged with a confidence level and points at a primary source you can refetch
— a datasheet section, a register struct in `esp-idf`, a vendor firmware file, a
GitHub issue with its actual comment thread (not just the title). Read `agent-playbook.md`
first if you are an agent (Hermes or otherwise) about to work in this repo.

## Files

| File | Covers |
|---|---|
| [`agent-playbook.md`](agent-playbook.md) | **Start here if you're an agent.** What's safe to do without hardware, escalation triggers to Claude Code, citation discipline. |
| [`hardware-esp32-c5.md`](hardware-esp32-c5.md) | ESP32-C5 silicon: USB-OTG software-support status, JTAG/pin sharing, strapping pins, vendor pinout cross-check. |
| [`peripherals-apa102-led.md`](peripherals-apa102-led.md) | APA102 protocol, brightness field, end-frame quirks, vendor firmware's LED recipe. |
| [`peripherals-lcd-tft-espi.md`](peripherals-lcd-tft-espi.md) | ST7735 panel, TFT_eSPI's missing C5 support and the vendored fix, SPI bus ordering. |
| [`toolchain-pioarduino.md`](toolchain-pioarduino.md) | Why the pioarduino platform fork is required, vendored board JSONs, Arduino-ESP32 3.x API drift catalog. |
| [`usb-serial-jtag-console.md`](usb-serial-jtag-console.md) | The C5's USB-CDC boot race, why the auth token can be lost, the `while(!Serial)` pattern and its known pitfalls. |
| [`open-questions.md`](open-questions.md) | **Read this before trusting any hardware claim in this repo.** Disputed/unverified claims found in the current branch, each with evidence and a concrete next test. |
| [`sources.md`](sources.md) | Full bibliography, grouped by topic, one line each. |

## Status semantics used throughout

| Tag | Meaning |
|---|---|
| `VERIFIED (hardware)` | Someone reported a specific observed result on real hardware (a serial log line, a visible LED/LCD state). Still: one report is not a regression test. |
| `VERIFIED (source)` | Confirmed by reading the primary source directly (register header, vendor firmware file, issue thread) in this session — not by running anything. |
| `PLAUSIBLE (unverified)` | Consistent with sources found, but no one has confirmed it against real hardware or the primary source directly. |
| `DISPUTED` | Two sources conflict, or the reasoning behind a claim doesn't hold up under closer reading. See `open-questions.md`. |

Built 2026-08-30 against branch `fix/c5-console-boot-race` (commit `b284e1b`) and
merged PRs #1–#3. Extended same-day with a second, hardware-verified pass (new
commits on the same branch) that corrected two claims from the first pass
(the SPI-vs-RMT `led_strip` backend, and the `usb_jtag_bridge_en` register's
actual documented direction) and added a live-hardware SPI diagnostic —
`firmware/src/diag/led_spi_diag.cpp` and its `pio run -e T-Dongle-C5-leddiag*`
build targets — reusable by anyone who has this board attached. See
`open-questions.md` #1's 2026-08-30 addendum for the full trail; check
`git log` for the current commit if this file is read from a later state of
the branch.
