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

The same path also never wired `ClaudeIntegration` (ANTS-5733).
It makes the tab current before its shell starts, so the tab-switch
handler passes `setShellPid` a PID of 0 and the poll stops. The status
label survived through the tracker, but the context gauge stayed hidden
until a tab switch.

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
- **WI-3** body calls `setShellPid(` after `startShell(` — without
  this, `ClaudeIntegration` never polls the restored focused tab and
  the context gauge stays hidden (ANTS-5733).
