# multi_window_session — ANTS-5032

Source-grep wiring test. Confirms two things about
`MainWindow` and `tab_order.txt` when more than one window is open in
the same process: a second window's constructor does not re-open tabs
the first window already restored, and the two windows do not race to
decide which tabs survive a restart.

## Invariants

INV labels are qualified by the roadmap item that added them.

| ANTS-5032 | Statement |
|----|-----------|
| 1  | `MainWindow::restoreSessions` declares a function-local `static bool` guard. Before the function's call to `SessionManager::loadTabOrder`, it returns when the guard is set, and sets it. |
| 2  | `mainwindow.cpp` calls `SessionManager::saveTabOrder(` exactly once. |
| 3  | That one call site is inside `MainWindow::saveProcessTabOrder`. |
| 4  | `MainWindow::saveProcessTabOrder`'s body walks `QApplication::topLevelWidgets()`. |
| 5  | `MainWindow::saveProcessTabOrder`'s body `qobject_cast`s each top-level widget to `MainWindow` and excludes `this` from the windows it folds in. |
| 6  | `MainWindow::saveAllSessions` calls `saveProcessTabOrder`. |
| 7  | `MainWindow::saveTabOrderOnly` calls `saveProcessTabOrder`. |
| 8  | `MainWindow::saveProcessTabOrder` adds each other window's `sessionTabIds()` to the list it saves. |
| 9  | `MainWindow::saveProcessTabOrder` does not filter windows on `isVisible` or `isHidden`. |

| ANTS-5118 | Statement |
|----|-----------|
| 1  | When `anotherWindowStaysOpen()` is true, `MainWindow::closeEvent` defers a close-down by one event-loop turn (`QTimer::singleShot(0 …)`). The close-down re-checks `anotherWindowStaysOpen()`, then closes every tab through `performTabClose`. |
| 2  | `MainWindow::anotherWindowStaysOpen` walks `QApplication::topLevelWidgets()`, casts each to `MainWindow`, skips `this`, and counts only visible windows. |

## Rationale

ANTS-5032: `MainWindow`'s constructor calls `restoreSessions()` with no
guard against a second construction in the same process. `File → New
Window` opens a second `MainWindow`, which runs the same constructor
and restores every saved tab a second time, under ids the first window
already owns.

Separately, `saveAllSessions` and `saveTabOrderOnly` each call
`SessionManager::saveTabOrder` with only their own window's tabs.
`tab_order.txt` is one file per process, not per window, so whichever
window saves last decides which tabs come back after a restart — the
other window's tabs are silently dropped from the next launch.

INV-1 locks the fix's restore half: a once-per-process guard so only
the first `MainWindow` in a process restores. INV-2 through INV-7 lock
the fix's save half: a single function, `saveProcessTabOrder`, becomes
the one place `SessionManager::saveTabOrder` is called, and it folds in
every open window's tabs (via `QApplication::topLevelWidgets()`) before
saving — so the file always reflects every window, not just the last
one to save. Hidden windows are folded in on purpose: a Quake-mode
window is hidden but still owns tabs, so only `this` is excluded, not
every non-visible widget.

ANTS-5118: the first window is built on the stack in `main.cpp`, so
closing it only hides it. It also owns the remote-control listener,
so it cannot be deleted. Its tabs and their shells used to run on
with no way back. Now a window closed while another visible window
stays open closes its tabs; deleting a tab ends its shell. The
close-down waits one event-loop turn, when a New Window window's
`deleteLater` would end its tabs, and checks again then. So when
every window is closed at once, the last one still saves every tab.
The last visible window keeps its tabs, because Qt then quits and the
restart must bring them back. A hidden Quake window does not count as
open, because Qt does not count it either.

## Scope

In scope: the once-per-process restore guard, and that both save paths
route through one function that gathers tabs across every open window
before writing `tab_order.txt`.

Out of scope: how `sessionTabIds` orders a window's tabs, and the
per-tab `.dat` scrollback files. `crash_safe_session_persist`,
`tab_rename_persist` and `claude_dot_restored_tabs` cover those.

## Why source-grep, not behavioural

No test in this project constructs a `MainWindow`. Its constructor
starts a shell in its first tab and sets up the remote-control server.
Two real windows would also need a real `tab_order.txt` on disk. The
invariants are wiring facts — a guard sits before a call, and one
function owns the save — so a source scrape checks them directly.
`crash_safe_session_persist/spec.md` makes the same choice for the
save timer.
