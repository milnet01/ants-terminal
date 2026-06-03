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
- **INV-16 (ANTS-1928)** — `advanceStability(prev, clampedTarget, current,
  nowMs)` folds one tick's clamped recommendation into the accrual state.
  Tier-lock window: once a non-current candidate tier appears, a reversion to
  `current` **within `kTierLockWindowMs`** does NOT reset accrued
  `ticksStable` (it holds), so a boundary-oscillation duty cycle down to
  ~1-in-3 ticks still reaches `kStableTicks`. A reversion that outlasts the
  window resets via the ANTS-1925 `kStableResetTicks` rule. Accrual is
  per-candidate-tier: a *changed* candidate restarts at 1 (a Sonnet→Opus then
  Sonnet→Haiku sequence never fires a switch off mixed evidence). Score
  hysteresis (the roadmap's part (a)) is **intentionally not applied**: making
  the current tier sticky would re-bias toward staying put, undoing ANTS-1930's
  symmetric-movement fix; the 90 s dwell gate (INV-6) is the anti-thrash
  backstop instead.
- **INV-6** — requires `msSinceLastSwitch >= kMinDwellMs`.
- **INV-7** — on act, `tierArg` is the lowercase alias (`haiku`/`sonnet`/`opus`)
  of the **clamped** target.
- **INV-8** — `clampToFloor` truth table over the full rec×floor matrix: the
  result is the higher-ranked of (rec, floor); Opus is never clamped.
- **INV-9** — security boundary: `tierArg` is always one of the three fixed
  enum aliases, derived only from `ModelRecommender::tierName` over the enum —
  never arbitrary text.
- **INV-15 (ANTS-1917)** — idle end-of-session suppression. When
  `idleElapsedMs >= 0` (controller supplies it only while the shell is Idle
  with a known `idleSinceMs`) AND `idleElapsedMs >=` the effective ceiling
  (`idleCeilingMs` if `>= 0`, else `kIdleEndOfSessionMs` = 3 min), `decide`
  appends `idle_end_of_session` and does not act — a tail switch would apply
  to no fresh work and would persist as the next session's default model.
  The `-1` sentinel (default Gate, no idle telemetry) never blocks, so a
  default-constructed Gate keeps the v1 7-token behaviour. Token is appended
  last in evaluation order (INV-9 of the near-miss taxonomy is never
  renumbered).
- **INV-16 (ANTS-1959)** — downgrade-at-idle suppression + long-ToolUse
  yield. (a) A **downgrade** (`tierRank(target) < tierRank(current)`) appends
  `downgrade_requires_active_work` and does not act whenever `idleElapsedMs >=
  0` (the shell is idle: end of work, between turns, winding down). Upgrades
  are never blocked by it (direction-sensitive). The `-1` sentinel keeps a
  default Gate at the v1 7-token behaviour. (b) The `focused_state_not_idle`
  veto YIELDS when `toolUseElapsedMs >= kLongToolUseMs` (10 s): a long
  foreground tool/build/test run is the one downgrade-safe active window. Net
  effect: downgrades fire only during sustained active work, never at idle.

## Method

Each negative case starts from a fully-satisfied `Gate` (which acts) and flips
exactly one field, so the assertion isolates that single gate. The baseline
case proves the satisfied gate acts, which is what makes the single-field flips
meaningful rather than vacuous.
