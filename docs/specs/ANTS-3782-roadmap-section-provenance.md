# ANTS-3782 — Roadmap section provenance: the `source_path` column and its reader

**Status:** spec draft (2026-08-01).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3782 (split out of ANTS-3766 at that spec's
cold-eyes loop 4, 2026-08-01, on the structural trigger — 1060 lines, and two
cold reads had failed to reach defects a third introduced).
**Blocked by:** ANTS-3757 (read half), ANTS-3765 (load half) — both shipped.
**Blocker for:** ANTS-3758 (publish + consumer cutover).
**Ships with:** [ANTS-3766](ANTS-3766-roadmap-migration-archives.md) — one
change, not a sequence. § 1.1 is why.
**Pairs with:** `docs/standards/roadmap-format.md` § 3.9 (archive rotation).

**Layman:** The roadmap database stores each chunk of the roadmap but not which
file it came from. This adds that one fact, so the old archive files can later
be written back where they belong instead of being folded into the main file.

## 1. Problem

ANTS-3758 regenerates `ROADMAP.md` **from the store**, not from a migration
plan. The plan is discarded at commit, so the plan-lifetime `sourceIndex`
[ANTS-3766](ANTS-3766-roadmap-migration-archives.md) § 2.4 assigns does not
reach the render: it indexes a `sources` vector nothing persists.

Nothing in the schema carries the fact instead. Verified against live source
rather than recalled: `RoadmapStore::createSchema()` (`src/roadmapstore.cpp`)
creates `section` with `section_id, project_id, slug, title, level, intro,
parent_id, UNIQUE (project_id, slug)`, and `RoadmapStore::SectionRow`
(`src/roadmapstore.h`) carries `slug, title, intro, level, parentId`. **No
table records which file a section came from, and no reader could return it if
one did.**

The consequence is not a missing feature but a silent loss. With archives
migrated and their origin unrecorded, ANTS-3758's first regeneration writes
every archived bullet back into `ROADMAP.md` — the archive un-rotated, in a
render that reports success. That is the same loss class ANTS-3766 exists to
close, arriving one layer further down.

### 1.1 Why this ships with ANTS-3766 and not after it

**Splitting the document is not splitting the work, and the two were decided
separately.** Sequencing the halves — read first, provenance later — was
considered when this split was taken and rejected: a read half that migrates
archives while writing provenance-free sections *is* the defect above, so
shipping it alone would introduce exactly what the pair prevents, for however
long the gap lasted.

So the halves land in one change. The split buys a document each cold reader
can hold, and nothing else; no build order changes, and neither spec is
implementable alone.

## 2. Surface

### 2.1 The column

One nullable column, added to ANTS-3756's `CREATE TABLE section`:

```sql
  source_path TEXT,          -- NULL = the live roadmap (ANTS-3782)
```

`NULL` for the live file rather than a literal path, because `NULL` is what the
live roadmap honestly means **and** it is what any row written before the
column reads back as. The two arguments coincide, so no backfill is ever needed
or attempted.

**Added to the DDL in place, at `kSchemaVersion` 1 — not by `ALTER TABLE`, and
not with a version bump.** Three facts, each measured rather than assumed:

- **There is no store to migrate.** Every file including `roadmapstore.h` is
  under `src/roadmap*` or in the test suite, so nothing user-facing can open a
  store and none exists outside a test's temp directory.
  *Measured:* `grep -rln 'roadmapstore\.h' src tests` → **8 files**, being
  `src/roadmapexport.cpp`, `src/roadmapmigrateload.h`, `src/roadmapstore.cpp`,
  and five `tests/features/roadmap_*/test_*.cpp`. No MCP verb, no dialog, no
  `mainwindow` translation unit. Enumerated from the source side rather than by
  confirming a list written here, so an omission would show up as an extra
  path rather than pass unnoticed.
- **There is nothing to hang an `ALTER` on.** `RoadmapStore::createSchema()`
  refuses a version above its own, no-ops on an equal one, and runs the DDL at
  0 — with no branch between. A store in between falls through to that same
  DDL, which is written **without** `IF NOT EXISTS` so a second creator fails
  loudly rather than succeeding silently; ANTS-3756 INV-15 makes the
  in-transaction `user_version` read the discriminator and says `IF NOT EXISTS`
  cannot be one.
- **A bump would invalidate the export goldens.**
  `tests/features/roadmap_export_roundtrip/golden/*.jsonl` each carry
  `"schema":1`, and `RoadmapExport::rebuildProject()` refuses a `meta` record
  whose schema differs from `RoadmapStore::kSchemaVersion`. *Measured:*
  `git grep -c '"schema":1' -- tests/features/roadmap_export_roundtrip/golden`
  → 3 files, 1 each.

So a bump would manufacture the upgrade case nothing implements, and regenerate
three goldens, in order to migrate zero stores.

**That freedom expires at ANTS-3758's cutover**, which is what first makes the
store reachable. **ANTS-3781** owns the upgrade path that is still missing; a
schema change landing after the cutover gets no such shortcut.

### 2.2 The reader

`SectionRow` gains one field and `readSection()` populates it:

```cpp
struct SectionRow {
    QString slug, title, intro;
    int     level;
    std::optional<qint64>  parentId;
    std::optional<QString> sourcePath;   // nullopt = the live roadmap
};
```

**A column with no reader is write-only, and INV-14 could not observe it.**
`SectionRow` is ANTS-3765 § 2.4's declaration and is the only typed path to a
section's contents, so without the field ANTS-3758 and every test would reach
past it into raw SQL — which is how a stored value and a writer's idea of it
stop being distinguishable.

`std::optional` rather than an empty `QString`, matching `parentId` in the same
struct. Here the `NULL` / `''` distinction **is** the semantics: `nullopt` is
the live roadmap, while `''` would be a root-relative path naming nothing.

### 2.3 The stored value is canonical and root-relative

`RoadmapMigrate::Source::path` is **absolute** — `findRoadmap()` builds it from
`fi.absoluteFilePath()` (`src/roadmapmigrate.cpp`) — so storing it verbatim
would write `/home/…/docs/roadmap/0.6.md`, making the store machine-specific
and never matching § 2.4's membership test. `RoadmapMigrateLoad::load()`
converts, against the `projectRoot` it already holds in `Options`, with **both
sides canonicalised**:

```cpp
QDir(QFileInfo(projectRoot).canonicalFilePath())
    .relativeFilePath(QFileInfo(source.path).canonicalFilePath())
```

`Source::path` itself stays absolute, so ANTS-3757's discovery invariants are
untouched.

**Both sides, and neither alone would do.** `absoluteFilePath()` does not
resolve symlinks, so a root reached through one yields
`/home/…/link/docs/roadmap/0.6.md`. Canonicalise only the root and the result
is a path computed *out of* the project (`../link/docs/…`); canonicalise only
the source and the mirror defect appears. Canonicalising both makes the
spelling a caller happened to pass stop mattering — which is also what keeps
this column consistent with `project.root`, keyed on `canonicalFilePath()` by
ANTS-3756 INV-8.

### 2.4 What the render re-splits on

ANTS-3758 re-emits the split by the rule `roadmap-format.md` § 3.9 already
fixes: a section whose `source_path` matches `docs/roadmap/<M>.<N>.md` belongs
in that archive, `NULL` belongs in the live file.

**No `isArchive` flag.** The path is the discriminator and § 3.9's naming regex
is the test — a boolean would be a second encoding of a fact the path already
carries, and the two would eventually disagree.

### 2.5 Preference calls and a rejected alternative

Both calls below are judgement rather than deduction, recorded at drafting on
2026-08-01 so a reviewer can overturn them instead of reverse-engineering them.

- **A column rather than letting ANTS-3758 infer archive membership.**
  Inference has nothing to work from once the plan is discarded (§ 1). The cost
  is an amendment to two shipped specs — real, and smaller than it looks: one
  DDL line and one write, because no store exists yet to migrate (§ 2.1).
- **`NULL` for the live file rather than a sentinel string.** A sentinel would
  need a value no real path can take, and every reader would have to know it.
  `NULL` already means "no source file recorded", which is what the live
  roadmap is under this design.

**Rejected: recover a section's source by string-matching its slug prefix,
storing nothing.** ANTS-3766 § 2.3 namespaces archive sections per source, so
`0-6-features` does encode its origin, and this needs neither `sourceIndex` nor
a column. It loses on three counts. A `Note` carries no section and the legend
belongs to the project rather than any section, so the derivation does not
cover the carriers that most need it. The encoding is ambiguous: a *live*
section legitimately titled `## 0.6 features` slugifies into the archive
namespace, and no parse can tell the two apart. And recovering structure by
parsing an identifier re-introduces the string-coupling the store's typed
columns exist to remove — a rule the store already applies to itself, keying
items on `(project_id, id_fold)` rather than on parsed text.

## 3. Invariants

**INV-14 is inherited from ANTS-3766, not renumbered** (`specs.md` § 5.5, and
the ANTS-3756 → ANTS-3761 precedent): it is cited by ANTS-3756 § 7 and
ANTS-3765 § 2.4, and an id that changed meaning across a split is worse than a
gap. New invariants continue that spec's sequence for the same reason — no
number in this document means anything different there.

- **INV-14** — the store's persisted discriminator is correct and
  machine-independent: after a load, `section.source_path` is SQL `NULL` for
  every section from the live roadmap and the **project-root-relative** path
  (`docs/roadmap/0.6.md`) for every section from an archive.
  *Test:* `tests/features/roadmap_migrate_load/` — loads ANTS-3766 § 6.1's
  baseline archive fixture and reads every section row back **through
  `readSection()`**; asserts `nullopt` for live sections, the exact relative
  string for archive sections, and that loading the same fixture again
  **through a differently-spelled but equivalent root** — a trailing slash, and
  a path through a symlink — stores byte-identical values.
  *Breaks when:* `load()` stores `sources[sourceIndex].path` verbatim, which is
  absolute, so the store works only on the machine that wrote it and § 2.4's
  membership test never matches anywhere else; **or** either side of § 2.3's
  computation is left un-canonicalised, for which the symlinked-root leg is the
  only detector — the trailing-slash leg passes against it, because `QDir`
  normalises that much on its own.
- **INV-15** — the column is reachable through the typed surface:
  `readSection()` returns what raw SQL holds, for both the `NULL` and the path
  case.
  *Test:* `tests/features/roadmap_store_schema/` — writes a section with a
  `source_path` and one without, then compares `readSection()->sourcePath`
  against a direct `SELECT source_path FROM section`, asserting `nullopt`
  matches SQL `NULL` and the string matches byte for byte.
  *Breaks when:* `SectionRow` gains the field but `readSection()`'s `SELECT`
  does not, so every row reads back `nullopt` and the live-roadmap leg passes
  while the archive leg fails. **Asserted against raw SQL, not a round-trip
  through the writer** — a writer compared with its own idea of the value
  cannot distinguish a stored value from a default, which is the ANTS-3767
  failure one column along.
- **INV-16** — this change does not move the schema version: a store created
  by this build reports `PRAGMA user_version` = 1, and the three export goldens
  still import.
  *Test:* `tests/features/roadmap_store_schema/` asserts the pragma;
  `tests/features/roadmap_export_roundtrip/` already imports the goldens and
  fails on a `meta` schema mismatch, so it is the second leg unchanged.
  *Breaks when:* `kSchemaVersion` is bumped for this column — which is the
  tempting move, and which requires an upgrade path that does not exist
  (ANTS-3781), against zero stores that would need one. This invariant is what
  makes § 2.1's argument a contract rather than a comment in a commit message.

## 4. RAM / build cost

No new build target, no new library, no new external dependency. One nullable
`TEXT` column: a root-relative archive path such as `docs/roadmap/0.6.md` is 22
bytes, and `NULL` costs only its row-header bit — so the column's cost is
bounded by (sections from archives) × ~22 bytes, and every live section pays
nothing. No row count is stated here because the store holds one row per
section **per migrated project** and this spec has no measurement of the
corpus-wide figure; the per-row cost is the honest bound and it is small enough
that the total cannot matter. One `std::optional<QString>` per `SectionRow`,
which is a stack temporary with the lifetime of a single read.

Nothing here is cached and nothing outlives its call.

## 5. Out of scope

- **The render itself** — ANTS-3758 owns emitting the split, and § 2.4 states
  only what it reads.
- **The schema-upgrade path** — ANTS-3781. Not needed until the cutover makes
  the store reachable (§ 2.1), and deliberately not built here so that this
  change stays a single DDL line.
- **The read half** — [ANTS-3766](ANTS-3766-roadmap-migration-archives.md):
  discovery, per-source format detection, slug identity, positions and
  partitions, `empty_source`, and the fixture set. This spec consumes its
  `sources` vector and its `sourceIndex` and redefines neither.
- **Backfilling an existing store** — there is none (§ 2.1), and `NULL` would
  be the correct value for every row of one if there were.

## 6. Tests

Feature tests, label `features;fast`: `tests/features/roadmap_migrate_load/`
(INV-14) and `tests/features/roadmap_store_schema/` (INV-15, INV-16). Both
directories exist and gain cases; no new bundle. Each invariant is verified RED
against the mutation its own clause names, before the implementation is
restored.

INV-14 shares ANTS-3766 § 6.1's baseline archive fixture rather than adding
one — the fixture's value is that its `docs/roadmap/0.5.md` and `0.6.md` are
byte-identical copies of this project's real archives, and a second hand-made
copy would drift from it.

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md)** — **schema change,
  APPLIED 2026-08-01** (before this split, under ANTS-3766's id). `section`
  carries `source_path TEXT` in its DDL at `kSchemaVersion` 1; `SectionRow` and
  `readSection()` carry the matching `sourcePath`; and its § 2.3
  upgrade-ownership sentence names ANTS-3781. Its § 7 amendment bullet cites
  ANTS-3766 and should be retargeted to this id.
- **[ANTS-3765](ANTS-3765-roadmap-migration-load.md)** — **write side, APPLIED
  2026-08-01** (same). Its § 2.6 section-resolution paragraph writes
  `source_path`, and its § 2.4 `SectionRow` comment cites `ANTS-3766 INV-14`;
  both should be retargeted to this id, the invariant number unchanged.
- **[ANTS-3766](ANTS-3766-roadmap-migration-archives.md)** — the parent.
  Its § 2.6 becomes a pointer here, its INV-14 is tombstoned in place, and its
  § 7 sheds the two amendment bullets above. It gains this spec's id in its
  header as the half it ships with.
- **ANTS-3758** — the consumer. § 2.4 is the contract it renders from; nothing
  here changes what it must do beyond giving it something to read.
- **`CHANGELOG.md`** — no entry of its own. This is user-invisible until
  ANTS-3758 lands the render, and it ships inside ANTS-3766's single change.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-08-01 | none — no reviewer dispatched | — | **Provenance row, not a review.** Split out of [ANTS-3766](ANTS-3766-roadmap-migration-archives.md) at that spec's loop 4, which stopped on the structural trigger rather than the cap: 1060 lines, and the loop's two CRITICALs plus INV-14's absence from the test plan were all loop-3-era material that two cold reads had failed to reach. This half was chosen as the seam because it is the only part with a persisted output and its own consumer. **INV-14 is inherited, not reflowed** (`specs.md` § 5.5); INV-15 and INV-16 are new and continue the parent's sequence so no number means two things across the pair. INV-15 answers a gap the parent's loop 4 found and fixed — a column with a writer and no reader — and INV-16 makes § 2.1's no-version-bump argument falsifiable, which it was not while it lived only in prose. **The parent's four loops do NOT transfer**: they were run against a document that no longer exists, so this spec runs the rule-14 gate from loop 1 on its own bytes. |
