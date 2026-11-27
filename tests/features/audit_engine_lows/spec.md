# Feature: audit engine context reads and ledger cache

## Invariants

**INV-1 — the context filter reads only source-sized files.** `applyFilter`
in `src/auditengine.cpp` skips a referenced file larger than
`kMaxContextFileBytes` instead of reading it whole.

**INV-2 — the false-positive ledger cache is locked.** `loadEntries` in
`src/falseposledger.cpp` holds a mutex around its function-static cache,
which the MCP worker and the GUI-thread review dialogs both reach.

**INV-3 — `test_` marks a test path only at a path-segment start.**
`computeConfidence` takes the test-path penalty for `src/test_x.cpp` and
`test_x.cpp`, not for `src/latest_x.cpp`.

**INV-4 — a clang-tidy finding in a path with a space is kept.**
`summariseClangTidyText` reports it; the `<N> warnings generated.` trailer
never matches the finding pattern.

**INV-5 — the sibling HTML report is derived only from a `.sarif` name.**
`summariseSarif` on a file not ending `.sarif` does not take `<dir>/.html`.

**INV-6 — a relative path under `tests/` or `test/` is a test path.**
`computeConfidence` takes the test-path penalty for `tests/x.cpp` and
`test/x.cpp`, as it does for `src/tests/x.cpp`.

## Rationale

The ANTS-5085 performance pass found both. `lineIsCode` already caps its read;
`applyFilter` did not. The ledger cache was commented single-threaded, but
since ANTS-2132 verbs run on the MCP worker.

## Test surface

`test_audit_engine_lows.cpp` reads both sources (located from the test's own
path) and checks the two function bodies for INV-1 and INV-2. INV-3 to INV-6
call the engine functions on temporary files.

## Regression history

- **ANTS-5085:** the two defects above. Locked by this spec.
- **ANTS-6622:** INV-3 to INV-5, from the 2026-11-06 reviews.
- **ANTS-6982:** INV-6, `/tests/` matched only with a leading slash.
