# Feature: a split pane's shell is tracked and released like a tab's

## Invariants

**INV-1 — a new split pane is tracked.** `MainWindow::splitCurrentPane` calls
`trackTerminalShell(newTerm)` after starting the pane's shell.

**INV-2 — closing a pane releases it.** `MainWindow::closeFocusedPane` calls
`releaseTerminalShell(focused)` before the pane is deleted.

**INV-3 — closing a tab releases every pane.** `MainWindow::performTabClose`
releases each `TerminalWidget` in the tab (`findChildren<TerminalWidget *>`),
not only the active one.

**INV-4 — release clears all three trackers.** `releaseTerminalShell` calls
`untrackShell`, `untrackBgShell` and `forgetShell`.

**INV-5 — an exited shell is still released.** `TerminalWidget::shellPid()`
reads -1 once the PTY has reaped the child, and keeps that meaning.
`TerminalWidget::startShell` records the forked PID in `m_lastShellPid`
(after the failure branch), exposed as `lastShellPid()`.
`releaseTerminalShell` uses `shellPid()` first and falls back to
`lastShellPid()` when it is -1, so a shell that exited before its pane or tab
closed leaves no state in the trackers for a reused PID to inherit.

## Rationale

`newTab` registered a shell with the per-tab Claude tracker and the
background-task tracker; `splitCurrentPane` registered nothing, so Claude in a
split pane never lit its tab dot. Tab close released only the active pane, and
pane close released nothing, so closed panes' PIDs stayed in three trackers.

## Test surface

`test_split_pane_shell_tracking.cpp` reads the MainWindow sources
(`ants_test::slurpMainWindow()`) and, for INV-5, `src/terminalwidget.h` /
`src/terminalwidget.cpp` (`SRC_TERMINALWIDGET_H_PATH`,
`SRC_TERMINALWIDGET_PATH`), and checks the function bodies as text. No
`MainWindow` is constructed.

`restoreSessions` keeps its direct `trackShell(` call, which
`claude_dot_restored_tabs` WI-1 pins by name.

## Regression history

- **ANTS-5079:** split panes were never tracked, and closing a pane or a split
  tab left PIDs in the trackers. Locked by this spec.
- **Exited shells:** `releaseTerminalShell` read only `shellPid()`, which is
  -1 after the child is reaped, so the `shellExited` close paths released
  nothing. Locked by INV-5.
