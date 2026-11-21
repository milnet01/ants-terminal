# ANTS-5078 — Stream scrollback and block exports to the file in slices — review loop log

Record for [`../specs/ANTS-5078-export-streaming.md`](../specs/ANTS-5078-export-streaming.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-17 | 1 (user decision: one reviewer, one pass) | 1 | 1 | 0 | 3 | Stopped at the user's one-pass cap: verified 5, fixed 5, dismissed 0. Q4: cast bytes carry wall-clock stamps, so the fixture pins commandStartMs/commandEndMs; setMaxScrollback clamps to 1000, so INV-5 names it; expected bytes come from the function bodies, since the old functions need a widget. Q2: INV-9 and section 7 disagreed on the comments naming the removed functions (copySelectionRich, contextMenuEvent). Q1 (from an open question): a cols() check misses two width changes in one turn, so recalcGridSize fails a running export. All five findings land on this new document. Mechanical pre-pass: spec_lint and check-doc-facts clean apart from two forward references. |
