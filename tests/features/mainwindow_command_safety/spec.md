# Feature: main-window commands and exports fail safely

## Invariants

**INV-1 — the audit review command quotes its prompt.** The handler for
`AuditDialog::reviewRequested` builds the `claude` command with
`shellQuote(prompt)`, not a hand-written pair of double quotes around the
results path.

**INV-2 — scrollback export reports failure.** The Export Scrollback handler
calls `TerminalWidget::startExport`, which reports a failure through
`captureFailed` (ANTS-5078). It shows its success message only on
`exportFinished` with `ok` true.

**INV-3 — a KWin script that cannot start is cleaned up.** `runKWinScript`
connects `QProcess::errorOccurred` for both `dbus-send` processes and removes
the temp script on `FailedToStart`.

**INV-4 — the SSH connect timer is guarded.** `onSshConnect` captures the
terminal as a `QPointer` and checks it before writing.

**INV-5 — the progress tab icon is rebuilt only when its state changes.** The
`TerminalWidget::progressChanged` handler records the last drawn state on the
tab's page widget and returns before painting when the new state matches it.
The record is per tab, not per pane, so a pane in a split tab still redraws
the icon after another pane cleared it.

## Rationale

The review command dropped the results path inside double quotes, so a `"` or
`$(` in it reached the shell. Scrollback export returned silently when the file
would not open and announced success after a short write. `runKWinScript`
connected only `finished`, which a process that fails to start never emits,
leaking the process and the temp script. The SSH timer captured a raw pointer
to a tab that can close inside the delay. The progress handler painted a new
pixmap and reset the tab icon on every OSC 9;4 sequence, although the icon
depends only on the state and a program may send one sequence per percent.

## Test surface

`test_mainwindow_command_safety.cpp` reads `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and checks the handler text. No `MainWindow` is
constructed.

## Regression history

- **ANTS-5079:** the defects above. Locked by this spec.
