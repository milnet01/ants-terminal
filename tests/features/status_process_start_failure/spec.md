# Feature: a helper process that fails to start is cleaned up

## Invariants

**INV-1 — the repo-visibility probe recovers from a failed start.**
`MainWindow::refreshRepoVisibility` connects `QProcess::errorOccurred`; on
`FailedToStart` it frees the process and clears
`m_repoVisibilityProbeInFlight` for the repo.

**INV-2 — the repo-visibility probe has a timeout.** `refreshRepoVisibility`
kills a `gh` still running after a fixed delay.

**INV-3 — the KWin position tracker frees a first stage that fails to start.**
In `KWinPositionTracker::setPosition`, the first `dbus-send`'s
`errorOccurred` handler calls `proc->deleteLater()` on `FailedToStart`.

**INV-4 — the KWin position tracker handles a second stage that fails to
start.** The second `dbus-send` (`proc2`) connects `QProcess::errorOccurred`
and removes the temp script.

## Rationale

`QProcess::finished` is not emitted for a process that fails to start. The
repo-visibility probe connected only `finished`, so a missing `gh` left its
in-flight flag set for the session, and a `gh` that never exited did the same.
The tracker's first stage leaked its `QProcess` on a failed start, and its
second stage had no handler at all.

## Test surface

`test_status_process_start_failure.cpp` reads `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and `src/kwinpositiontracker.cpp` beside it, and
checks the function text.

The first `m_repoVisibilityProbeInFlight.remove(repoRoot)` stays inside
ANTS-1554's pragma block (`build_warning_repo_visibility_null_deref`); the new
handler clears the flag by assignment.

## Regression history

- **ANTS-5080 / ANTS-5081:** the defects above. Locked by this spec.
