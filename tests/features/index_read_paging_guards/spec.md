# Feature: read_region, pagination and index cache guards

## Invariants

**INV-1 — read_region clips an oversized first line.** `ReadRegion::extract`
keeps the first line of a range even when it exceeds `maxBytes`, but clipped
to `maxBytes`, with `truncated:true`.

**INV-2 — an auto-sized page always advances.**
`PaginationEngine::pageBullets` with no explicit limit returns at least one
row when rows remain, so `next_offset` is always past `offset`.

**INV-3 — codebase_index checks its cache write.** The cache write in
`src/codebaseindex.cpp` checks the write and `QSaveFile::commit()`.

## Rationale

The ANTS-5103 performance pass found all three: a single-line huge file came
back whole, a first row larger than the page left `next_offset` equal to
`offset`, and a failed cache commit was ignored.

## Test surface

`test_index_read_paging_guards.cpp`: INV-1 and INV-2 call the functions;
INV-3 reads `src/codebaseindex.cpp` (located from the test's own path).

## Regression history

- **ANTS-5103:** the three defects above. Locked by this spec.
