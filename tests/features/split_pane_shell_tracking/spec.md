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

## Rationale

`newTab` registered a shell with the per-tab Claude tracker and the
background-task tracker; `splitCurrentPane` registered nothing, so Claude in a
split pane never lit its tab dot. Tab close released only the active pane, and
pane close released nothing, so closed panes' PIDs stayed in three trackers.

## Test surface

`test_split_pane_shell_tracking.cpp` reads `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and checks the function bodies as text. No
`MainWindow` is constructed.

`restoreSessions` keeps its direct `trackShell(` call, which
`claude_dot_restored_tabs` WI-1 pins by name.

## Regression history

- **ANTS-5079:** split panes were never tracked, and closing a pane or a split
  tab left PIDs in the trackers. Locked by this spec.
