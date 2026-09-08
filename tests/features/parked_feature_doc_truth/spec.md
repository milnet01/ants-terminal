# Feature: the subsystem map does not describe a parked actuator as live

## Problem

`docs/subsystems.md` is the map a session reads to learn what a
subsystem does. Its `modelautoswitch` entry says:

> The live actuator (`ClaudeStatusBarController::refreshAutoModelSwitch`,
> timer-driven) injects `/model <tier>` on `decide(...).act`, appends a
> pending ledger record, and suppresses the Shape A chip when enabled.

None of the first two happens. `ModelAutoSwitch::kAutoSwitchActuatorParked`
is true, and `refreshAutoModelSwitch` returns on it before the
`sendToPty`, before the handshake, before the firing-side surfacing and
before the ledger append. The decision and its near-miss telemetry still
run; nothing downstream of the decision does.

The entry compounds it by presenting the config flag as what holds the
feature back — "Default-OFF via `Config::claudeAutoModel().switch_enabled`;
S2 gates the default-ON flip". The park comment in the source says the
opposite about that flag's power: the guard exists so "a
config-migration bug that flips `claude.auto_model_switch` on can never
re-arm it".

So the map tells a reader the actuator fires, and tells them the config
is what stops it. Both are wrong, and the second is the more dangerous:
it invites someone to reach for the config to test a code path that is
disarmed in code.

## Contract

While `kAutoSwitchActuatorParked` is true, the `modelautoswitch` entry
in `docs/subsystems.md` MUST say the actuator is parked, and MUST NOT
describe it as live.

The check is bidirectional. When the constant becomes false, the entry
MUST stop saying parked — otherwise un-parking the feature leaves the
map wrong in the other direction, which is the same defect with the
sign flipped.

## Invariants

**INV-1 — the constant is readable.** `kAutoSwitchActuatorParked` is
found in `src/modelautoswitch.h` with a boolean value. Guards the two
below against passing vacuously when the parse fails.

**INV-2 — parked implies the map says so.** When the constant is true,
the `modelautoswitch` entry contains "parked".

**INV-3 — parked implies the map does not claim a live actuator.**
When the constant is true, the entry does not contain "The live
actuator".

**INV-4 — un-parked implies the map stops saying parked.** When the
constant is false, the entry does not contain "parked".

## Scope

### In scope
- The `modelautoswitch` entry in `docs/subsystems.md`, against the
  constant in `src/modelautoswitch.h`.

### Out of scope
- Whether the feature should be un-parked. ANTS-2195 parked it and
  owns that decision; this is about the map matching it.
- The rest of `docs/subsystems.md`. One entry, one constant.
- Other documents describing the switcher. If one exists it has the
  same problem, and finding them is a documentation sweep rather than
  this contract.

## Regression history

- **ANTS-1735 / ANTS-1890:** built the switcher and wrote the entry,
  when the actuator did fire.
- **ANTS-2195:** parked the actuator in code, so a config flip could
  not re-arm it. The map was not updated.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "map describes a deliberately parked feature as live
  (`kAutoSwitchActuatorParked` returns before injection and before the
  ledger append)". Verified against source — the return also precedes
  the handshake and the surfacing — and fixed. Locked by this spec.
