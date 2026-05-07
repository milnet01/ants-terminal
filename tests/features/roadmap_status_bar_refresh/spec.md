# Status-bar widget refresh on launch

## Surface

`MainWindow` ctor wires `refreshRoadmapButton()` and
`refreshRepoVisibility()` to status-bar refresh paths. Per
ANTS-1160 §9 — and the cold-eyes-traced root cause of the v0.7.77
regressions — both widgets read `shellCwd()`, which reads
`/proc/<shellPid()>/cwd`. The PID is set by `startShell()` AFTER
`newTab()`'s `setCurrentIndex()` triggers `onTabChanged(0)` for
the first time. So wiring only to `onTabChanged` is insufficient:
the first call sees `shellPid()==0`, `shellCwd()` returns empty,
the widget hides itself, and nothing reschedules.

The fix is additive — both widgets must ALSO be invoked from:

  - the status-bar 2-second timer (`m_statusTimer`), so the
    widget re-evaluates within 2 s of any state change; AND
  - the startup `singleShot(0)` lambda, so the widget re-evaluates
    once `startShell()` has finished its first turn of the event
    loop (typically ~1 ms after construction).

Same fix shape that ClaudeBgTaskTracker / refreshBgTasksButton
already follow correctly. The Task List dialog refresh (third
member of the ANTS-1160 §9 trio) was fixed separately in
commit `2f5d470` via the `ClaudeTaskListTracker::poll()` pattern.

## Invariants

- **INV-1**: `mainwindow.cpp` contains the literal line
  `connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshRoadmapButton)`.
- **INV-2**: `mainwindow.cpp` contains a `singleShot(0` lambda
  body that calls `refreshRoadmapButton()`.
- **INV-3a**: `mainwindow.cpp` contains the literal line
  `connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshRepoVisibility)`.
- **INV-3b**: `mainwindow.cpp` contains a `singleShot(0` lambda
  body that calls `refreshRepoVisibility()`.
- **INV-4**: `refreshRoadmapButton` is still ALSO called from
  `onTabChanged` (regression-guard — fix is additive, not a
  replacement). Same for `refreshRepoVisibility`.
- **INV-5**: `docs/standards/status-bar.md` exists and contains
  the State-category-widget refresh contract addendum naming
  `shellCwd`, `m_statusTimer`, and `singleShot(0)`.

## Source

  - ANTS-1160 §9 — folded-in regression list with the architectural
    shape spelled out.
  - ANTS-1160 §10 criterion 9 — acceptance gate.
  - Pre-existing precedent: v0.6.29 review-button regression of
    the identical class; ANTS-1158 task list shipped with the
    same bug shape and was fixed in commit `2f5d470` via the
    `claudetasklist.cpp::poll()` mtime-gated rescue.

Manual review gate: not required. Source-grep INVs above are
sufficient — the fix is mechanical and the failure mode is binary
(widget appears at launch / doesn't).
