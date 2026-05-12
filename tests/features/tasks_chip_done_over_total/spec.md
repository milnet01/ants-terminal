# tasks_chip_done_over_total — ANTS-1246

Pins the Tasks chip's progress semantics: chip text reads
`☰ <done>/<total>`, hide condition is `done >= total`, chip stays
visible during in-progress runs.

Source-grep harness against `claudestatuswidgets.cpp` —
behavioural drive of the Qt-widget refresh slot would require a
live MainWindow + status bar + ClaudeTaskListTracker harness, out
of scope at this tier. The tracker's own count methods are covered
by the existing `claude_task_list*` tests; this test pins the chip
formatter that consumes them.

## Invariants

| # | Lane | Statement |
|---|------|-----------|
| 1 | text | `refreshTasksButton` calls `setText` with `"☰ %1/%2"` and `arg(done).arg(total)` — the numerator is the completed count, not the legacy `total - unfinished`. |
| 2 | hide | The hide-gate predicate is `done >= total` (full-completion); the legacy `unfinished <= 0` predicate is gone. |
| 3 | anti-regression | The deleted `unfinished <= 0` predicate must not reappear in the file — a refactor that re-introduces it re-breaks ANTS-1246's user-visible behavior. |
| 4 | tooltip | The tooltip's `X done, Y running, Z outstanding` breakdown remains in place (unchanged by ANTS-1246; informational dialog content stays the same). |

## Acceptance

Exit 0 = all 4 invariants hold.

Wired into `ants_add_gui_bundle(test_claude …)` at top-level
`CMakeLists.txt`. No per-feature CMakeLists.txt.

## Re-open conditions

- The chip text format changes (e.g. adds an icon variant). Update INV-1.
- A future API rename swaps `completedCount()` → some other name on `ClaudeTaskListTracker`. Update INV-1's grep.
- A flicker-suppression grace window lands (per spec § 8). Add an INV for the grace timer.
