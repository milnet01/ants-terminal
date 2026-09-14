# Feature: session saves use per-process temp files and check their writes

## Invariants

**INV-1 — each process writes its own temp file.** `saveSession` and
`saveTabOrder` in `src/sessionmanager.cpp` name their temp file with the
process id, so two running copies never write the same temp path.

**INV-2 — the tab order is renamed into place only after a full write.**
`saveTabOrder` checks that the whole body was written and flushed before
renaming; otherwise it removes the temp file and keeps the previous
`tab_order.txt`.

**INV-3 — the orphan sweep still finds the temp files.** The sweep of old temp
files matches both the per-process names and the older fixed names.

## Rationale

The ANTS-5106 performance pass found that two copies of Ants shared
`<name>.tmp`, so their periodic saves could tear a session file, and that
`saveTabOrder` renamed a short write over the good file.

## Test surface

`test_session_save_guards.cpp` reads `src/sessionmanager.cpp` (located from the
test's own path).

## Regression history

- **ANTS-5106:** the defects above. Locked by this spec.
