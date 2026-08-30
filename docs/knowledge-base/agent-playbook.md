# Agent playbook

For any agent (local model or otherwise) working in this repo. Read this before
`AGENTS.md`'s task-specific gotchas — this file is about *how* to work here, not
*what* the hardware does.

## Why this file exists

Two prior sessions in this repo (local models, not Claude) produced commits and
comments that state hardware behavior as settled fact when it was actually a
guess, or cite a real source while getting its conclusion backwards. See
`open-questions.md` for the specific, sourced examples — read it now if you
haven't; it's short and every entry there is a pattern worth not repeating.

The common thread in all five cases: the model found *a* source, matched it to
its existing mental model of how the fix should work, and stopped — without
reading the source's own caveats (an issue's later comments, not just its title;
a register's actual documented purpose, not just its name) or checking it against
the one source that's cheapest to verify and hardest to argue with: what the
device's own manufacturer already ships and presumably already tested.

## Before you touch hardware-facing code

1. **Check `open-questions.md` first.** If the code you're about to change is
   already flagged there, don't extend or "clean up" the existing comment's
   reasoning — the reasoning may be the bug. Either leave it alone and
   escalate, or follow that entry's specific next-test.
2. **Prefer the vendor's own firmware over any secondary source.** For this
   board, that's `github.com/Xinyuan-LilyGO/T-Dongle-C5` — pin numbers, register
   pokes (or their absence), and default recipes (LED brightness, backlight
   polarity) there are ground truth from the people who built the board and
   presumably tested their own demo. If your fix requires a register write the
   vendor's own firmware doesn't make, that's a signal to double-check *why*
   before committing it, not proceed anyway because it compiles.
3. **When you fetch a GitHub issue for a technical claim, fetch the comments,
   not just the issue body.** A tool that renders a GitHub issue page as
   markdown often won't include dynamically-loaded comments. Use
   `api.github.com/repos/<owner>/<repo>/issues/<n>` for the body and
   `.../issues/<n>/comments` for the full thread — both work unauthenticated on
   public repos. An issue's *title* and *closing label* often summarize the
   *end state*, not the specific fix that got there — see `open-questions.md`
   #2 for a case where that distinction mattered.
4. **A register header (`soc/*_struct.h` in `esp-idf`) beats a doc-site
   paraphrase beats a blog post beats your own inference from the register's
   name.** `usb_jtag_bridge_en` *sounds* like it should mean "release the JTAG
   pins" — the actual documented bit behavior is closer to the opposite. Names
   are not specifications.
5. **Don't write "verified on hardware" unless you personally observed the
   specific result this session** (a serial log line you were shown, a
   photo/description of the LED/LCD state someone gave you). If you're
   restating what a commit message or comment already claims, say that
   explicitly ("per commit `X`, reported as...") rather than re-asserting it as
   your own independently-confirmed fact — the next reader (model or human)
   needs to be able to tell "I checked this" from "someone before me said they
   checked this."

## What's safe to do without hardware access

No physical board needed, low blast radius, fine to self-verify by compiling
(`pio run --environment T-Dongle-C5` and `-S3`) and reading the diff carefully:

- New DuckyScript interpreter commands/tests (Tier 1/2), following
  `docs/programming-style.md` and the existing static-array/no-malloc pattern.
- New C2 REST API routes, following the existing auth-token gating pattern.
- Sample `.dd` payloads under `payloads/`.
- Documentation: README/AGENTS.md/knowledge-base edits, **provided** any factual
  claim you add is either (a) sourced the way this knowledge base is, or (b)
  clearly marked as your own reasoning/plan rather than an observed fact.
- Non-hardware refactors that preserve behavior (renames, extracting genuinely
  3×-repeated logic, per `docs/programming-style.md`).
- Fixing the two docs-only nits in `open-questions.md` (#3 citation pairing, #5
  GPIO5 label) — low-risk, purely textual.

## Escalate to Claude Code (or wait for a human with the hardware) when

- The change touches JTAG/pin-mux/register-level code (`hal.cpp`'s
  `usb_jtag_bridge_en` write, anything using `soc/*_struct.h` directly) —
  `open-questions.md` #1 is exactly this kind of change, and it cannot be
  resolved by reasoning alone; it needs a reflash-and-observe cycle.
  the fix would only be verifiable by re-flashing physical hardware and you
  have no way to get that feedback in-session. Don't mark such a task "done"
  because it compiled — say explicitly that it's unverified and needs a
  hardware pass.
- SPI/bus-sharing timing or init ordering (`Storage::init()` vs
  `Display::init()` in `main.cpp`) — see `open-questions.md` #2 for why
  reordering these is riskier than it looks.
- Anything that requires reconciling a datasheet/TRM register table against
  code, where getting it backwards produces no compile error and a plausible
  but wrong comment (the exact failure mode in `open-questions.md` #1).
- Any change to boot sequencing (`main.cpp`'s `setup()` early section) — the
  USB-CDC wait, the auth-token print, and the SPI-bus-before-LCD ordering all
  live there and interact.
- You've spent real effort and still can't find a primary source that directly
  supports a needed claim — don't fill the gap with a guess stated as fact;
  flag it as an open question the way this file's siblings do, and hand it off.

## Standing rule: promote hardware findings out of `.hermes/plans/` every session

The most detailed hardware ground-truth this project has ever produced lived in
`.hermes/plans/*.md` — gitignored, local to one machine (`open-questions.md` #6).
Confirmed with the repo owner on 2026-08-30: **this is a standing rule, not a
one-off.** Before a hardware bring-up plan file is superseded, abandoned, or
would otherwise stop being your active working file, promote its durable
findings — verified-working subsystems, root-caused bugs, register/pin facts,
anything another session would otherwise have to rediscover — into a tracked
location: `docs/knowledge-base/`, `AGENTS.md`, `README.md`'s status table, or a
commit message, as appropriate to what the finding is. "I wrote it in the plan
file" does not count as done. This applies equally whether you're Hermes or
Claude.

## Git conventions

Already covered in `CONTRIBUTING.md` — follow it as-is (branch naming,
Conventional Commits, one logical change per commit, cite the hardware source
in the commit body). Nothing in this file overrides it.
