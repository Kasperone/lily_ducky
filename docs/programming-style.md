# Programming Style — Torvalds-Inspired

> "Bad programmers worry about the code. Good programmers worry about data structures
> and their relationships." — Linus Torvalds

Adapted for this project from `work_pilot_agent`'s style rules (TypeScript original)
to embedded C/C++. Same principles, native idioms.

Write code that is simple, obvious, and correct. Optimize for the reader, not the
writer. Every abstraction, every indirection, every helper must earn its place by
making intent clearer at the call site. When in doubt, write the straightforward thing.

## Data Structures First

Design the shape of your data before writing logic. Get the types, the memory layout,
and the data flow right — the code follows naturally. When a function is hard to write,
the problem is usually the data model, not the algorithm.

- Define the structs, enums, and buffer layouts before implementing logic
  (`DefineEntry`, `VarEntry`, `LcdState` in this codebase are the pattern)
- If a function needs complex branching, ask whether the data could be restructured
  to eliminate it
- Prefer flat, fixed-size arrays over pointer-chasing (no heap in the interpreter —
  see AGENTS.md)

## Eliminate Special Cases (The "Good Taste" Principle)

The hallmark of good code: reframe the problem so edge cases dissolve into the
general case. If you have an `if` that handles "the first item" or "the empty case"
differently, look for a formulation where that special case disappears.

- Early returns for preconditions are fine — they remove the special case from the
  main flow
- Unify dual targets through one capability flag (`CFG_HAS_USB_HID`) rather than
  scattering `#ifdef TARGET_X` through every file — pin maps and feature gating
  resolve in `config.h` alone
- When you see an if/else where both branches do almost the same thing, unify them

## Earn Your Abstractions

No wrapper, helper, or utility unless it makes intent clearer at the call site.
Three similar lines of code is better than a premature abstraction. If you must read
the implementation to understand what an abstraction does, it is a bad abstraction.

- Don't create a helper used in one place — inline it
- Don't wrap a library call in a "service" that adds no logic (the `Storage` API is
  justified: it hides a real transport difference, SDMMC vs SPI, behind one interface)
- Extract only when: (a) the same logic appears 3+ times, or (b) the extraction gives
  a name that communicates intent better than the raw code
- Delete unused abstractions immediately — no "might need it later"

## Short Functions, Shallow Nesting

One screen, one job. If a function doesn't fit in ~30 lines or needs more than 2–3
levels of nesting, split it. Deep nesting is a design smell — refactor, don't indent.

- Max 2–3 levels of indentation in any function
- Use early returns to flatten nested if/else chains
- Each function does one thing — if you'd describe it with "and", split it
- On MCU: short functions are also cheap — inlining is the compiler's call, not yours;
  don't hand-inline for micro-optimization without measuring

## Comments: WHAT and WHY, Never HOW

If code needs a comment explaining HOW it works, simplify the code instead. Comments
explain purpose and motivation — the non-obvious WHY behind a decision.
Self-documenting code with clear names beats commented clever code.

- Good: `// 1-bit mode: only D0 used, D1/D2 don't need to be wired`
- Good: `// MSC stubs return zeros — enabling this without raw SD I/O would expose a phantom drive`
- Bad: `// loop through array and check each element`
- Never comment out dead code — delete it (git remembers)
- Hardware constraints deserve a WHY comment with a source (datasheet section,
  LilyGO file, Espressif issue number); implementation steps do not

## Pragmatism Over Dogma

Use whatever construct is clearest for the situation. Don't follow patterns or
"best practices" that add complexity without adding value. Rules exist to serve code
quality — when a rule makes code worse, break it.

- `String` in one web-server handler is fine; a hot loop in the interpreter stays
  `char[]` — match the tool to the context
- A mutable local reassigned once is clearer than a chain of ternaries
- Don't reorganize working code to match a pattern it doesn't need
- Arduino idioms (`setup`/`loop`, `Serial.printf`) are the platform's conventions —
  follow them rather than fighting them

## Fix Root Causes

Understand why something broke before fixing it. Never patch symptoms. If a bug
appears, trace it to its origin — the fix belongs there, not at the point where the
symptom shows up.

- Read the error message and full build log before changing code
- If a fix requires a comment explaining the workaround, you probably fixed the wrong thing
- When a type error appears, fix the type at its source — don't cast at the usage site
- Flaky hardware behaviour means incomplete understanding — check the schematic and
  timing, don't add `delay()` and hope
- When fixing a bug, check sibling call paths for the same flaw — fix the class,
  not just the reported site (e.g. ESCAPE/BACKSPACE scancode swap)

## Optimize for the Reader

Code is read 10x more than written. Naming, structure, and flow should be obvious to
someone seeing the code for the first time.

- Variable names: descriptive enough that you never wonder what they hold, short
  enough that they don't clutter (`_capturePos`, `_osDetectRunning`)
- Function names: verb + noun (`loadPayload`, `generateToken`), never ambiguous
  (`process`, `handle`, `doStuff`)
- File organization: related things stay close — one directory per module
  (`hal/`, `interpreter/`, `c2/`, `display/`, `storage/`), header beside source
- Prefer explicit over implicit — a few extra lines of obvious code beats a clever
  one-liner
