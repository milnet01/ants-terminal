# Feature: main-window commands and exports fail safely

## Invariants

**INV-1 — the audit review command quotes its prompt.** The handler for
`AuditDialog::reviewRequested` builds the `claude` command with
`shellQuote(prompt)`, not a hand-written pair of double quotes around the
results path.

**INV-2 — scrollback export reports failure.** The Export Scrollback handler
writes through `QSaveFile`, checks `commit()`, and shows a failure message when
the file cannot be opened or the write fails.

**INV-3 — a KWin script that cannot start is cleaned up.** `runKWinScript`
connects `QProcess::errorOccurred` for both `dbus-send` processes and removes
the temp script on `FailedToStart`.

**INV-4 — the SSH connect timer is guarded.** `onSshConnect` captures the
terminal as a `QPointer` and checks it before writing.

## Rationale

The review command dropped the results path inside double quotes, so a `"` or
`$(` in it reached the shell. Scrollback export returned silently when the file
would not open and announced success after a short write. `runKWinScript`
connected only `finished`, which a process that fails to start never emits,
leaking the process and the temp script. The SSH timer captured a raw pointer
to a tab that can close inside the delay.

## Test surface

`test_mainwindow_command_safety.cpp` reads `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and checks the handler text. No `MainWindow` is
constructed.

## Regression history

- **ANTS-5079:** the four defects above. Locked by this spec.
