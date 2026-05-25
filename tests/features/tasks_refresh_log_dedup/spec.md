# tasks_refresh_log_dedup — ANTS-1859

Pins the `tasks/refresh` debug-line dedup key in
`ClaudeStatusBarController::refreshTasksButton`
(`src/claudestatuswidgets.cpp`).

ANTS-1854 added a per-tick dedup signature so a quiet poll that
re-derives identical state emits nothing. The original key included
the transcript **mtime** (`preMtimeMs`) on the theory that "a real
transcript append advances the mtime → new signature → logged". But
Claude streams its output into the JSONL transcript continuously, so
the mtime ticks on *every* poll even when the task list is unchanged
— the mtime-keyed signature therefore re-logged a near-identical line
every ~2 s for the whole session (ANTS-1859, observed in a live
debug-log review 2026-05-25).

The fix: drop `preMtimeMs` from the dedup **key** so the line logs
only on a genuine task-state transition (counts / visibility branch).
The mtime stays in the logged *line* (the ANTS-1458 latency column),
so a real change still carries its mtime context.

The sibling `refreshBgTasksButton` already keys correctly
(`focused | path | running | total | branch`, no mtime) — this brings
the tasks path in line with it.

Source-grep harness against `claudestatuswidgets.cpp` — behavioural
drive of the Qt-widget refresh slot needs a live MainWindow + status
bar + tracker, out of scope at this tier (same rationale as
`tasks_chip_done_over_total`).

## Invariants

| # | Lane | Statement |
|---|------|-----------|
| 1 | dedup-key | The tasks `const QString sig = …;` construction in `refreshTasksButton` does NOT reference `preMtimeMs` — the transcript mtime is excluded from the dedup key. |
| 2 | key-state | The same sig construction still keys on the task state: `.arg(total)`, `.arg(unfinished)`, `.arg(inProgress)`, `.arg(pending)`, `.arg(done)` and the visibility `branch` — so dedup is not over-broadened into hiding real transitions. |
| 3 | latency-preserved | `preMtimeMs` is still emitted in the `ANTS_LOG` line (the ANTS-1458 `mtime=` latency column), so a genuine change still records its mtime context. |
| 4 | sig-width | The tasks sig `QStringLiteral` format string is 7 placeholders (`%1`…`%7`), not the pre-fix 8 — i.e. exactly one field (mtime) was removed, nothing else dropped. |

## Acceptance

Exit 0 = all 4 invariants hold.

Wired into `ants_add_gui_bundle(test_claude …)` at top-level
`CMakeLists.txt`. No per-feature CMakeLists.txt.

## Re-open conditions

- The dedup signature is reworked (e.g. a coalescing time-window
  replaces the transition key). Update INV-1/INV-4.
- A future field is legitimately added to the key (raising the
  placeholder count). Update INV-4's expected width.
- The latency `mtime=` column is removed from the log line. Update
  INV-3.
