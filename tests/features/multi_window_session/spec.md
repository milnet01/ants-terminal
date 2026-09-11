# multi_window_session — ANTS-5032

Source-grep wiring test. Confirms two things about
`MainWindow` and `tab_order.txt` when more than one window is open in
the same process: a second window's constructor does not re-open tabs
the first window already restored, and the two windows do not race to
decide which tabs survive a restart.

## Invariants

INV labels qualified `ANTS-5032-INV-N`.

| #  | Statement |
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
