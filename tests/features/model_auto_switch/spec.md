# Feature: autonomous model-switch gate (`ModelAutoSwitch::decide`)

Part of **ANTS-1735** (Shape B of ANTS-1226). Full design + invariants live in
[`docs/specs/ANTS-1735.md`](../../../docs/specs/ANTS-1735.md) §2.3. This test
pins the **pure decision helper** — the part that decides *whether* and *to
what* the autonomous controller switches Claude Code's model. It is value-in /
value-out, so it is exercised with no live terminal.

## Scope

`src/modelautoswitch.{h,cpp}`:

- `clampToFloor(rec, floor)` — a recommendation below the configured floor is
  raised to the floor; the floor never blocks an upgrade (Opus is never clamped
  down).
- `decide(Gate)` → `{act, tierArg}` — the gate that the controller evaluates
  each tick.

Out of scope (covered elsewhere / gated on spikes): the live actuator wiring
(PTY injection, `idleSinceMs` / `lastUserKeystrokeMs` stamping, chip
suppression — INV-14), the effectiveness ledger (INV-10..12), and the
`model_switch_stats` MCP verb (INV-13).

## Invariants

- **INV-1** — `decide` returns `act=false` when `enabled` is false, regardless
  of other fields.
- **INV-2** — `act=false` when `focusedState != Idle`. One case per non-Idle
  `ClaudeState` value (NotRunning, Thinking, ToolUse, Compacting).
- **INV-3** — `act=false` when `composerEmpty` is false.
- **INV-4** — acts only when `clampToFloor(recommended, floor) != current`.
- **INV-5** — requires `ticksTargetStable >= kStableTicks`, counted against the
  **clamped** target — a recommendation that clamps to current never
  accumulates (no livelock / churn).
- **INV-6** — requires `msSinceLastSwitch >= kMinDwellMs`.
- **INV-7** — on act, `tierArg` is the lowercase alias (`haiku`/`sonnet`/`opus`)
  of the **clamped** target.
- **INV-8** — `clampToFloor` truth table over the full rec×floor matrix: the
  result is the higher-ranked of (rec, floor); Opus is never clamped.
- **INV-9** — security boundary: `tierArg` is always one of the three fixed
  enum aliases, derived only from `ModelRecommender::tierName` over the enum —
  never arbitrary text.

## Method

Each negative case starts from a fully-satisfied `Gate` (which acts) and flips
exactly one field, so the assertion isolates that single gate. The baseline
case proves the satisfied gate acts, which is what makes the single-field flips
meaningful rather than vacuous.
