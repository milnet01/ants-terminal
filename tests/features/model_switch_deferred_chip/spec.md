# model_switch_deferred_chip — feature-conformance spec (ANTS-1915)

## Purpose
Give the user a "switch model without interrupting" path through Ants's own
UI. Clicking the status-bar model chip while Claude is mid-generation must NOT
send `/model` immediately — Claude Code's TUI would leave it unsubmitted in the
composer until the user presses Escape (interrupting the turn). Instead the
switch is deferred and fired automatically when the owning shell next goes Idle.

Scope note: the *typed* `/model` case (user types it in Claude Code's composer
mid-generation) stays blocked on Claude Code exposing composer state via MCP —
Ants cannot robustly reconstruct composer content from raw keystrokes. That
dependency is tracked separately on the roadmap. This feature covers only the
Ants-owned chip-click path, where Ants knows both the intent and the state.

## Behaviour
- Chip click while focused shell state ∈ {Thinking, ToolUse, Compacting}:
  record `m_deferredChipTier` + `m_deferredChipShellPid`, set a "queued"
  tooltip, return focus to the terminal, and do NOT send `/model`.
- Chip click while Idle / NotRunning: send `/model` immediately (unchanged
  pre-1915 behaviour).
- On `shellStateChanged(pid)` → `maybeFireDeferredChipSwitch(pid)` fires the
  deferred switch iff a deferral is pending for that pid AND the shell is now
  Idle AND its terminal still exists. The terminal is resolved by shellPid
  (not focus), so the switch lands on the correct PTY even if the user moved
  to another tab.

## Invariants
- **INV-1** — The chip-click handler defers (sets `m_deferredChipTier`) when the
  focused shell is generating, and returns before the immediate `sendToPty`.
- **INV-2** — `maybeFireDeferredChipSwitch` is connected to the tracker's
  `shellStateChanged` signal (event-driven; independent of whether the
  autonomous switcher is enabled).
- **INV-3** — `maybeFireDeferredChipSwitch` no-ops unless the pid matches the
  pending deferral AND `shellState(pid).state == Idle`.
- **INV-4** — On fire, the terminal is resolved by shellPid (loop over
  `m_terminalAtTabProvider`), the deferral state is cleared, and the ANTS-1890
  override cool-down (`m_lastOverrideMsByProject`) is seeded so the autonomous
  switcher does not immediately undo the user-initiated switch.
- **INV-5** — If no terminal owns the pid at fire time (tab closed), the
  deferral is dropped without sending `/model`.

## Test files
- `test_deferred_chip_wiring.cpp` → `test_claude` (source-grep wiring guard,
  mirroring the ANTS-1735 INV-14 actuator-wiring guard pattern; the live
  controller path is not unit-testable without a TerminalWidget + tracker
  harness, same constraint noted in auto_switch_surfacing/spec.md).
