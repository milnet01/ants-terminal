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

**INV-4 — the store holds only ASCII ids.** `RoadmapParse::isAsciiId` is the
one check. A non-ASCII id is refused by `RoadmapStore::putItem`,
`RoadmapStore::reassignItemId`, `RoadmapStore::relateCrossProject` (its
`dstIdFold`), and `RoadmapExport::rebuildProject` (an item's `id`, a
cross-project relationship's `dst`). Each refusal writes nothing. The
migration refuses the project in `Loader::matchItems`, naming the bullet's
file and line, as it does for a duplicate id; it does not drop the label.

## Rationale

The store's `id_fold` is `GENERATED ALWAYS AS (lower(id))`. The ANTS-5087
performance pass found that the export rebuild and the migration keyed ids
with `QString::toLower()`, which folds non-ASCII letters the store's column
does not, so the key missed the row.

**One premise in that reasoning was wrong and is corrected here**
(ANTS-5304): it read "SQLite's `lower()` changes only ASCII letters". That is
true of a DEFAULT SQLite build, not of SQLite. A build carrying the ICU
extension folds accented letters in `lower()` too. So `foldId` and the
`id_fold` column agree on a default build and diverge on an ICU one. The
column is computed by SQLite while `relationship.dst_id_fold` is bound from
the C++ fold, and `UNIQUE (project_id, id_fold)` would mean different things
on the two builds. INV-4 closes that: with ASCII ids only, every build folds
alike, and no schema change is needed.

## Test surface

`test_roadmap_id_fold.cpp` compares `foldId` with SQLite's `lower()` on an
in-memory database, over ASCII ids only (INV-1); asserts `foldId`'s treatment
of non-ASCII against written-out values with no database (INV-3); and reads
the four sources (located from the test's own path) for `toLower()` on an id
(INV-2). For INV-4 it drives each write point on a real store with `Ä-1`
and checks the refusal and that no row landed: the rebuild through an export
with the id substituted, the migration through a GFM roadmap whose bold
lead-in is accented prose.

## Regression history

- **ANTS-5087:** the mismatched folds above. Locked by this spec.
- **ANTS-5304 (2026-09-22):** INV-1 compared `foldId` against SQLite's
  `lower()` for `Ä-1` and `ÉTÉ-7`. That asserts the SQLite build rather than
  the code, and failed the Mageia_10 OBS build of v0.7.110 while
  openSUSE_Tumbleweed, openSUSE_Leap_16.0 and the local suite all passed.
  INV-1 is now scoped to the ids the format admits, and INV-3 carries the
  non-ASCII claim without a database.
- **ANTS-5304 (2026-11-09):** the store's write points accepted a non-ASCII
  id. INV-4 refuses one. Measured that day: no id in the live store held a
  non-ASCII character, so the refusal breaks no project.
