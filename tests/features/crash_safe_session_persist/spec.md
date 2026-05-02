# crash_safe_session_persist — ANTS-1159

Source-grep wiring test. Confirms that `MainWindow` saves session
state on a periodic timer + tab-event hooks, not only at
`closeEvent`.

## INV map

INV labels qualified `ANTS-1159-INV-N`.

| #  | Statement |
|----|-----------|
| 1  | `m_sessionSaveTimer` member exists in `mainwindow.h` and is initialised with interval `30000`. |
| 2  | The timer's `timeout` signal is connected to `MainWindow::saveAllSessions`. |
| 3  | `saveTabOrderOnly` (or equivalently-named member) is declared in `mainwindow.h` and defined in `mainwindow.cpp`. |
| 4  | `saveTabOrderOnly` is called from within `MainWindow::newTab` (post-`addTab`). |
| 5  | `saveTabOrderOnly` is called from within `MainWindow::performTabClose` (post-`removeTab`). |
| 6  | `saveTabOrderOnly` is connected to a `tabMoved` signal on the tab bar. |
| 7  | `MainWindow::closeEvent` stops `m_sessionSaveTimer` before calling `saveAllSessions` (so the timer can't re-enter mid-shutdown). |
| 8  | `MainWindow::closeEvent` STILL calls `saveAllSessions()` (no regression of the existing path). |
| 9  | `saveTabOrderOnly` short-circuits on `!sessionPersistence()` and on the 5 s uptime floor (mirrors `saveAllSessions`). |

## Why source-grep, not behavioural

The feature is a wiring change — three new signal hookups + one
new member + one new slot. The behavioural semantics (timer
fires every 30 s, tab events trigger save) are guaranteed by
Qt itself once the wiring is correct. A behavioural test would
need to drive `QApplication` for 30 + s under `ctest`, which the
project's `fast` lane explicitly avoids. The source-grep
approach matches `tests/features/ui_state_persistence/`
(ANTS-1150) and similar wiring tests.

## Anti-regression scope

The test does NOT assert the exact 30-s value (so a future tuning
to e.g. 60 s wouldn't trigger a red). It asserts the value is
present. If the interval becomes runtime-configurable, replace
INV-1 with a fence on the config-key wiring instead.
