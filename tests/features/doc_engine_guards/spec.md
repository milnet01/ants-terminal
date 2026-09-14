# Feature: documentation engine fence rule and cache write

## Invariants

**INV-1 — the TOC writer uses the shared fence rule.** `patchTocRegion` in
`src/doclint.cpp` decides what is inside a code fence with
`MarkdownScan::fenceMask`, the rule the checks that report TOC gaps use; no
private fence scanner remains in the file.

**INV-2 — docs_index checks its cache write.** The cache write in
`src/docsindex.cpp` checks the write and `QSaveFile::commit()` and reports a
failure.

## Rationale

The ANTS-5099 performance pass found both. The private fence rule closed a
backtick fence on a tilde line and missed fences nested under list items, so
the writer could see different headings from the finding it was fixing.

## Test surface

`test_doc_engine_guards.cpp` reads both sources (located from the test's own
path).

## Regression history

- **ANTS-5099:** the two defects above. Locked by this spec.
