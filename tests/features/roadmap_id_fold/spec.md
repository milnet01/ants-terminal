# Feature: one id fold, matching the store's column

## Invariants

**INV-1 — for an id the roadmap format admits, `RoadmapParse::foldId` folds
as SQLite's `lower()` does**, so `foldId` of an id equals the `id_fold` column
the store generates for it. Those ids are ASCII: § 3.5 admits a
`[A-Za-z0-9_-]` prefix with a `-\d+` suffix, and a stable id is
`^[A-Za-z][A-Za-z0-9_-]+$`.

**The ASCII scope is the invariant, not an omission** (ANTS-5304). `lower()`
is not one function — a default SQLite folds only ASCII, an ICU-enabled build
folds accented letters too — so above ASCII this claim asserts which SQLite
was linked. At or below ASCII every build agrees.

**INV-3 — `foldId` lowercases ASCII `A`-`Z` and leaves every other code point
unchanged.** A property of `foldId` alone, asserted against written-out
expected values rather than against a database, so no SQLite build can change
the answer.

**INV-2 — every C++ id-fold key goes through `foldId`.** `roadmapexport.cpp`,
`roadmapmigrateload.cpp`, `roadmapmigrate.cpp` and `roadmapstore.cpp` build
no id key with `QString::toLower()`.

## Rationale

The store's `id_fold` is `GENERATED ALWAYS AS (lower(id))`. The ANTS-5087
performance pass found that the export rebuild and the migration keyed ids
with `QString::toLower()`, which folds non-ASCII letters the store's column
does not, so the key missed the row.

**One premise in that reasoning was wrong and is corrected here**
(ANTS-5304): it read "SQLite's `lower()` changes only ASCII letters". That is
true of a DEFAULT SQLite build, not of SQLite. A build carrying the ICU
extension folds accented letters in `lower()` too. So `foldId` and the
`id_fold` column agree on a default build and diverge on an ICU one — which
leaves a residual question this spec does not settle: the column is computed
by SQLite while `relationship.dst_id_fold` is bound from the C++ fold, and
`UNIQUE (project_id, id_fold)` means different things on the two builds.
Unreachable while ids stay ASCII, which the format requires. **ANTS-5304**
carries it.

## Test surface

`test_roadmap_id_fold.cpp` compares `foldId` with SQLite's `lower()` on an
in-memory database, over ASCII ids only (INV-1); asserts `foldId`'s treatment
of non-ASCII against written-out values with no database (INV-3); and reads
the four sources (located from the test's own path) for `toLower()` on an id
(INV-2).

## Regression history

- **ANTS-5087:** the mismatched folds above. Locked by this spec.
- **ANTS-5304 (2026-09-22):** INV-1 compared `foldId` against SQLite's
  `lower()` for `Ä-1` and `ÉTÉ-7`. That asserts the SQLite build rather than
  the code, and failed the Mageia_10 OBS build of v0.7.110 while
  openSUSE_Tumbleweed, openSUSE_Leap_16.0 and the local suite all passed.
  INV-1 is now scoped to the ids the format admits, and INV-3 carries the
  non-ASCII claim without a database.
