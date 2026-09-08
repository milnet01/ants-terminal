# Feature: the switch-confirm poll reuses the cached Config

## Problem

ANTS-2116 added `cachedConfig()` to `claudestatuswidgets.cpp` because
"each `Config()` ctor does open + readAll + JSON parse", and the 2 s
status tick was doing five to seven of them per tick. It reloads only
when `config.json`'s mtime or size changes.

Two functions on the user-typed `/model` confirm path construct a fresh
`Config` instead, and they run in a loop.

`maybeAutoConfirmUserModelSwitch` runs on the 2 s tick. It reads
`claudeAutoModelConfirmUserSwitch()` from a fresh `Config`, and when no
dialog is on screen it arms a burst of `pollUnarmedSwitchConfirm` at
`kSwitchConfirmPollMs` for `kSwitchConfirmMaxPolls` attempts. Each of
those attempts reads the same setting from another fresh `Config`.

The burst's budget is just under the tick interval, so the next tick
arms the next burst: in the steady state the loop never stops. One tick
plus one full burst is seventeen open-read-parse cycles every two
seconds — more than ANTS-2116 removed, on the same timer, in the same
file.

`pollUnarmedSwitchConfirm` reads config twice more in the branch it
takes when the burst's budget runs out, which is once per burst rather
than once per attempt. Those are in scope here because they are on the
same repeating path, not because their rate is the problem.

It is the default configuration. `claudeAutoModelConfirmUserSwitch`
defaults to true, and the burst is armed by the *absence* of a dialog,
so this runs whenever an Ants window has a focused terminal — Claude
running or not.

## Contract

Both functions MUST read the setting through `cachedConfig()`.

The tradeoff `cachedConfig()` carries is already accepted on this
timer by ANTS-2116: a Settings change is picked up when the file's
mtime or size moves, which Settings Apply causes because it rewrites
`config.json` through `QSaveFile`.

## Invariants

**INV-1 — `maybeAutoConfirmUserModelSwitch` constructs no `Config`.**

**INV-2 — `pollUnarmedSwitchConfirm` constructs no `Config`.**

**INV-3 — both still read the setting.** Each body references
`claudeAutoModelConfirmUserSwitch`, so the fix cannot pass by deleting
the read.

**INV-4 — `cachedConfig` still exists and is mtime-and-size guarded.**
The invariant above is only worth anything if the helper it routes to
is still the caching one.

## Scope

### In scope
- The two functions that run in the poll loop.

### Out of scope
- `sendUnarmedConfirm`, `performModelSwitchHandshake` and
  `emitSwitchSurfacing`, which also construct a fresh `Config` but fire
  once per user action rather than in a loop. Left deliberately: the
  defect here is the repetition, not the construction.
- The burst itself. Re-arming to catch a dialog that renders between
  ticks is ANTS-1955's design and is not what this changes.
- The `recentOutput` scrollback scan each poll also performs. Same
  loop, different cost, and not measured here.

## Regression history

- **ANTS-2116:** introduced `cachedConfig()` and routed the three tick
  refreshers through it.
- **ANTS-1951 / ANTS-1955:** added the unarmed-confirm tick check and
  its poll burst, each reading config directly.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "~8 config open+parse per second on the default configuration,
  re-adding the cost ANTS-2116 removed". Verified against source: the
  rate is one tick read plus sixteen burst reads per two seconds, and
  the toggle's default is true. Fixed. Locked by this spec.
