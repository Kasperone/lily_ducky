# Contributing — git conventions

This repo is maintained with AI agents as first-class contributors. The rules below
keep history shaped so an agent (or human) can always answer: *what changed, why,
and where does work continue?*

## Branches

| Pattern | Purpose | Example |
|---|---|---|
| `main` | Stable, builds cleanly. Never force-push. | — |
| `hw/<board>` | Hardware port / board support work | `hw/t-dongle-c5-port` |
| `feat/<name>` | New feature (interpreter command, C2 route…) | `feat/ble-hid` |
| `fix/<name>` | Bug fix | `fix/define-word-boundary` |
| `docs/<name>` | Docs only | `docs/readme-refresh` |
| `refactor/<name>` | Behaviour-preserving restructuring | `refactor/storage-api` |

- Branch off `main`; keep branches short-lived (one concern each).
- Delete merged branches.

## Commits

Conventional Commits, imperative mood, ≤ 72-char subject:

```
<type>: <what changed — imperative, no trailing period>

<why, not what. Body wraps at ~72 chars. Reference hardware docs,
issue numbers, or Espressif links when the change is driven by an
external constraint.>
```

Types: `feat` · `fix` · `hw` · `docs` · `refactor` · `test` · `chore`.

Rules that matter here:

1. **One logical change per commit.** A hardware port and a README rewrite are two
   commits, never one.
2. **Explain the constraint, not the diff.** "ESP32-C5 has no USB-OTG (esp-usb#371)"
   beats "add ifdefs".
3. **No secrets, ever.** No WiFi passwords other than the public lab default, no
   tokens, no paths under `~`. `git filter-repo` is not a cleanup plan.
4. **Build before you commit.** `pio run` must pass for the env you touched
   (both envs if you touched shared code).

## Working agreements for agents

- Read `AGENTS.md` before touching firmware; `docs/programming-style.md` before
  writing any code.
- Never commit directly to `main`; open a branch, push, and summarize.
- Prefer `patch`-style minimal edits over file rewrites unless a full rewrite is the
  actual task.
- When a change is driven by hardware reality (pin map, silicon limitation), cite the
  source (LilyGO repo file, datasheet section, Espressif issue) in the commit body.
- Leave the tree as you found it: no stray build artifacts, no reformatted-but-untouched
  files.

## Releases & versioning

Tags: `v<major>.<minor>.<patch>` on `main` when a milestone is demonstrable on
hardware (e.g. `v0.3.0` — first successful payload execution on a real host).
Pre-hardware work stays `v0.x`.
