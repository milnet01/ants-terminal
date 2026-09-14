# Feature: one id fold, matching the store's column

## Invariants

**INV-1 — `RoadmapParse::foldId` folds as SQLite's `lower()` does.** It
lowercases ASCII `A`-`Z` and leaves every other character unchanged, so
`foldId` of an id equals the `id_fold` column the store generates for it.

**INV-2 — every C++ id-fold key goes through `foldId`.** `roadmapexport.cpp`,
`roadmapmigrateload.cpp`, `roadmapmigrate.cpp` and `roadmapstore.cpp` build
no id key with `QString::toLower()`.

## Rationale

The store's `id_fold` is `GENERATED ALWAYS AS (lower(id))`, and SQLite's
`lower()` changes only ASCII letters. The ANTS-5087 performance pass found
that the export rebuild and the migration keyed ids with `QString::toLower()`,
which also folds non-ASCII letters, so an id such as `Ä-1` mis-linked.

## Test surface

`test_roadmap_id_fold.cpp` compares `foldId` with SQLite's `lower()` on an
in-memory database for ASCII and non-ASCII ids, and reads the four sources
(located from the test's own path) for `toLower()` on an id.

## Regression history

- **ANTS-5087:** the mismatched folds above. Locked by this spec.
