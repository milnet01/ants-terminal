# Feature: background tasks dialog reads what it checked, and reads finished output once

## Invariants

**INV-1 — the output tail opens the validated path.** `tailPathSafe` in
`src/claudebgtasksdialog.cpp` returns the canonical path it checked, and
`tailFile` opens that path rather than the one it was given.

**INV-2 — finished output is read once.** `ClaudeBgTasksDialog::rebuild`
keeps a finished task's output in `m_finishedOutput` and does not call
`tailFile` for it again; `rewatch` watches only running tasks' output files.

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
