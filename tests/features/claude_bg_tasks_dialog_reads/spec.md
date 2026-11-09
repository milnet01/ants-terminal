# Feature: background tasks dialog reads what it checked, and reads finished output once

## Invariants

**INV-1 — the output tail opens the validated path.** `tailPathSafe` in
`src/claudebgtasksdialog.cpp` returns the canonical path it checked, and
`tailFile` opens that path rather than the one it was given.

**INV-2 — finished output is read once.** `ClaudeBgTasksDialog::rebuild`
keeps a finished task's output in `m_finishedOutput` and does not call
`tailFile` for it again; `rewatch` watches only running tasks' output files.

**INV-3 — one copy of each output.** `rebuild` drops `m_finishedOutput`
entries for tasks the tracker no longer lists. The skip-identical check
keeps a hash of the page (`m_lastHtmlHash`), not the page HTML.
Checked by `KeepsOneCopyOfOutput`.

## Rationale

The ANTS-5092 performance pass found both. The check canonicalised the path
but the open used the original, so a symlink swapped in between pointed the
read elsewhere. Each rebuild re-read every task's output, finished ones
included.

## Test surface

`test_claude_bg_tasks_dialog_reads.cpp` reads `src/claudebgtasksdialog.cpp`
(located from the test's own path) and checks the three function bodies.

## Regression history

- **ANTS-5092:** the two defects above. Locked by this spec.
- **ANTS-5203:** the cache kept output of tasks long gone, and the page HTML
  was held whole beside the viewer's own document. Locked by INV-3.
