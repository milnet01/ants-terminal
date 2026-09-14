# Feature: audit engine context reads and ledger cache

## Invariants

**INV-1 — the context filter reads only source-sized files.** `applyFilter`
in `src/auditengine.cpp` skips a referenced file larger than
`kMaxContextFileBytes` instead of reading it whole.

**INV-2 — the false-positive ledger cache is locked.** `loadEntries` in
`src/falseposledger.cpp` holds a mutex around its function-static cache,
which the MCP worker and the GUI-thread review dialogs both reach.

## Rationale

The ANTS-5085 performance pass found both. `lineIsCode` already caps its read;
`applyFilter` did not. The ledger cache was commented single-threaded, but
since ANTS-2132 verbs run on the MCP worker.

## Test surface

`test_audit_engine_lows.cpp` reads both sources (located from the test's own
path) and checks the two function bodies.

## Regression history

- **ANTS-5085:** the two defects above. Locked by this spec.
