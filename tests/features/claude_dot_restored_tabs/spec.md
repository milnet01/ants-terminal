# Feature: per-tab Claude state dot survives Ants restart

Canonical bug: ANTS-1375 (user report 2026-05-14; root-caused
2026-05-15 via the throttled per-tab branch-tracer that landed in
the same ticket).

## Problem

After an Ants restart, tabs restored from session-persistence
showed no per-tab Claude state dot, even when the restored shell
had a running Claude Code session. The bottom-bar status widget
still showed "Claude: idle" for the focused tab — but the
indicator dot stayed dark on every tab.

Root cause: `MainWindow::restoreSessions` starts the shells for
restored tabs via `startShell`, but never calls
`m_claudeTabTracker->trackShell(terminal->shellPid())`. The
`newTab` and `newTabForRemote` paths both do. So
`ClaudeTabTracker::m_shells` never contains the restored shells'
PIDs, and `shellState(pid)` falls through to the default
`ShellState{state: NotRunning}` for every restored tab — which
the indicator-provider lambda translates to `Glyph::None` (no
dot).

The bottom-bar still works because the tab-switch handler at
`mainwindow.cpp:4340` calls `m_claudeIntegration->setShellPid(...)`
on focus, wiring up `ClaudeIntegration` independently of the
tracker.

Fix: one line. After `startShell` succeeds in `restoreSessions`,
call `trackShell(terminal->shellPid())` (same shape as `newTab`).

## What the C++ test pins

Source-grep on `MainWindow::restoreSessions` body via the
`slurpFunctionBody` helper (`srcgrep.h`):

- **WI-1** body contains a `trackShell(` call — without this, the
  regression returns silently and the per-tab dots stay dark on
  restored shells.
- **WI-2** body still contains the `startShell(` call — defensive
  against accidental hoist of the trackShell call out of the
  restoredTabs loop (where it must run AFTER startShell sets the
  shell PID).
