# Feature: Single-source-of-truth Claude state resolver (ANTS-1873)

Bug fixed (2026-05-26): the per-tab dot (`ColoredTabBar` indicator
provider in `claudestatuswidgets.cpp`) and the bottom status-bar label
(`ClaudeStatusBarController::apply`) read state from two different
sources — the tracker's `ShellState` vs. cached scalars updated from
integration signals. They drifted in both directions: tab dot showed
"idle" while the bar said "thinking", and vice versa for "prompting".

Fix: extract `claudestate::{Resolved, Display, fromShell, forPid,
forFocused, display}` in `src/claudestateresolver.{h,cpp}` and route
both surfaces (plus the autonomous switcher gate, ANTS-1735 INV-2)
through it. By construction the two surfaces cannot disagree.

See `docs/specs/ANTS-1873.md` for the full contract.

## Coverage

- INV-1..INV-7 — behavioural tests of the pure helper.
- INV-8, INV-9, INV-10 — source-grep against
  `src/claudestatuswidgets.cpp`.
- INV-11 — regression-locked by the existing claude-status test
  suites continuing to pass.
