# ANTS-3782 — Roadmap section provenance: the `source_path` column and its reader

**Status:** spec draft (2026-08-01).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3782 (split out of ANTS-3766 at that spec's
cold-eyes loop 4, 2026-08-01, on the structural trigger — 1060 lines, and two
cold reads had failed to reach defects a third introduced).
**Blocked by:** ANTS-3757 (discovery + parse), ANTS-3765 (the load half that
performs this write) — both shipped. *"Read half" is reserved throughout for
ANTS-3766, this spec's sibling.*
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

`NULL` for the live file rather than a sentinel string — § 2.5 is the argument.
One consequence belongs here: it is also what any row written before the column
reads back as, so no backfill is ever needed or attempted.

**Added to the DDL in place, at `kSchemaVersion` 1 — not by `ALTER TABLE`, and
not with a version bump.** Three facts, each measured rather than assumed:

- **There is no store to migrate.** Every file that `#include`s `roadmapstore.h` is
  under `src/roadmap*` or in the test suite, so nothing user-facing can open a
  store and none exists outside a test's temp directory.
  *Measured:* `grep -rln 'roadmapstore\.h' src tests` → **8 files**, being
  `src/roadmapexport.cpp`, `src/roadmapmigrateload.h`, `src/roadmapstore.cpp`,
  and five `tests/features/roadmap_*/test_*.cpp`. No MCP verb, no dialog, no
  `mainwindow` translation unit. Enumerated from the source side rather than by
  confirming a list written here, so an omission would show up as an extra path
  rather than pass unnoticed. **The grep proves *direct* includers**; the claim
  it supports needs one more step, which holds because the three `src/` entries
  are the store, its exporter and the migration loader, and nothing outside
  `src/roadmap*` calls any of them — no MCP verb, dialog or menu action reaches
  a store, so none can exist on a user's disk.
- **There is nothing to hang an `ALTER` on.** `RoadmapStore::createSchema()`
  refuses a version above its own, no-ops on an equal one, and runs the DDL at
  0 — with no branch between. A store at a version this build does not recognise falls through to that same
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
    int     level = 0;
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
struct: the `NULL` / `''` distinction is load-bearing (§ 2.5), and a type that
cannot express it would lose the distinction at the reader.

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

**Two ways the conversion can fail, and neither may be stored.**
`QFileInfo::canonicalFilePath()` returns the **empty string** for a path that
does not resolve — a source deleted between discovery and load, or a symlink
loop — and an empty root would relativise every archive to nonsense. And a
source that canonicalises *outside* the project root yields a `../…` value
§ 2.4's membership test can never match, which would be a silently unplaceable
section. So `load()` refuses the project — `ok == false`, nothing written,
consistent with ANTS-3765 INV-1's one-project-one-transaction rule — when
either canonicalisation is empty **or** the computed relative path begins
`../`. Refusing rather than storing is the same call ANTS-3756 INV-8 makes for
`project.root`, where Qt's empty return would otherwise fuse two projects into
one.

### 2.4 What the render re-splits on

ANTS-3758 re-emits the split on the path itself: a section whose `source_path`
is `docs/roadmap/` + a name matching

```
^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$
```

belongs in that archive; `NULL` belongs in the live file. **That regex is
[ANTS-3766](ANTS-3766-roadmap-migration-archives.md) § 2.2's, cited and not
re-decided** — it is tighter than `roadmap-format.md` § 3.9's stated one, which
admits the zero-padded `00.07.md` its own prose forbids, and the two must be
the same test on both sides of the store or a file discovery accepted would be
a file the render cannot place. Nothing else matches: `0.6.1.md`, `00.07.md`
and `v0.7.md` are not archives at either end.

**No `isArchive` flag.** The path is the discriminator and the regex is the
test — a boolean would be a second encoding of a fact the path already carries,
and the two would eventually disagree.

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
gap.

**The new invariants are numbered from 26, past ANTS-3756's highest, and that
is deliberate rather than tidy.** Continuing ANTS-3766's sequence would have
given 15 and 16 — and **ANTS-3756 already has an INV-15**, cited by name in
§ 2.1 above and written into the very file this change touches
(`src/roadmapstore.cpp` carries a bare `// INV-15 — this connection is the
creator`). Two of the three invariants below are tested in
`tests/features/roadmap_store_schema/`, whose test names are `Inv<N>…` in
ANTS-3756's numbering, so a bare `Inv15` there would resolve two ways
permanently. Numbering past that range costs a gap in this document and buys an
id that means one thing everywhere it appears.

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
- **INV-26** — the column is reachable through the typed surface:
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
  cannot distinguish a stored value from a default, which is the ANTS-3767 failure one column along: `item`'s `lanes`, `evidence`
  and `extras` each had a column and no way to write it, so every call reported
  success while the columns held their DDL defaults.
- **INV-27** — this change does not move the schema version: a store created
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
`TEXT` column: a root-relative archive path such as `docs/roadmap/0.6.md` is 19
bytes (`printf '%s' 'docs/roadmap/0.6.md' | wc -c`), and `NULL` costs only its row-header bit — so the column's cost is
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
- **Backfilling an existing store** — § 2.1's last paragraph; there is nothing
  to backfill and nothing to decide.

## 6. Tests

Feature tests, label `features;fast`: `tests/features/roadmap_migrate_load/`
(INV-14) and `tests/features/roadmap_store_schema/` (INV-26, INV-27). Both
directories exist and gain cases; no new bundle. Each invariant is verified RED
against the mutation its own clause names, before the implementation is
restored.

INV-14 shares ANTS-3766 § 6.1's baseline archive fixture rather than adding
one — the fixture's value is that its `docs/roadmap/0.5.md` and `0.6.md` are
byte-identical copies of this project's real archives, and a second hand-made
copy would drift from it.

## 7. Cross-doc impact

- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md)** — **schema change, applied
  to that spec's text on 2026-08-01** (before this split, under ANTS-3766's id);
  **the code change lands with this item** — `src/roadmapstore.cpp` still has no
  `source_path`, which is what § 1 states. In the spec: `section` carries
  `source_path TEXT` in its DDL at `kSchemaVersion` 1, `SectionRow` and
  `readSection()` carry the matching `sourcePath`, and its § 2.3
  upgrade-ownership sentence names ANTS-3781. Its § 7 amendment bullet was
  retargeted to this id on 2026-08-01 and is not outstanding.
- **[ANTS-3765](ANTS-3765-roadmap-migration-load.md)** — **write side, applied
  to that spec's text on 2026-08-01; the code change lands with this item.**
  Its § 2.6 section-resolution paragraph performs the write and quotes § 2.3's
  expression; its § 2.4 `SectionRow` carries `sourcePath` and cites this spec's
  INV-14. **Both citations were retargeted on 2026-08-01 and neither is
  outstanding.** One correction *was* outstanding and is now applied: that
  paragraph carried the un-canonicalised
  `QDir(projectRoot).relativeFilePath(source.path)` — the form § 2.3 rejects
  and INV-14's symlinked-root leg is the only detector for. It was introduced
  when the write was first specified and survived because § 2.3's
  canonicalisation was added later, in ANTS-3766's loop 4, without sweeping the
  paragraph that consumes it.
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
| 1 | 2026-08-01 | 2 cold `general-purpose` lanes, one shared byte-identical context packet | C 1 · H 2 · M 5 · L 7 — 15 verified, 1 dismissed | All 15 fixed. **C1 — both lanes, and it was live fix collateral rather than a drafting defect:** ANTS-3765 § 2.6 still carried the un-canonicalised `QDir(projectRoot).relativeFilePath(source.path)` while § 2.3 here mandates canonicalising both sides. The canonicalisation was added during ANTS-3766's loop 4 and never swept into the paragraph that performs the write, so an implementer working from the write-side spec would have produced exactly the value INV-14's symlinked-root leg exists to catch. Fixed in ANTS-3765, with this spec named as the expression's owner. **H1 — § 7 told the implementer to retarget two citations that this session had already retargeted**, which sends them to redo landed edits and hides the one item genuinely outstanding. **H2 — the new invariants collided with ANTS-3756's INV-15**, which this document cites by name and which `src/roadmapstore.cpp` carries as a bare `// INV-15` comment; two of the three tests land in `roadmap_store_schema`, where test names are `Inv<N>…` in ANTS-3756's numbering, so they are renumbered **INV-26 / INV-27** past that range. INV-14 keeps its inherited number. Also: § 2.4 made § 3.9's naming regex the discriminator without ever quoting it (now cited from ANTS-3766 § 2.2, the single statement); § 2.3 defined no failure mode for `canonicalFilePath()` returning empty or resolving outside the root (now a refusal); "read half" named two different specs; the no-store claim rested on direct includers alone; and the `NULL`-versus-sentinel decision was argued in three places (now § 2.5 alone). Four open questions the lanes raised were resolved by lookup, not by another loop: `Options` does carry `projectRoot`, ANTS-3766 has shed both amendment bullets, `SectionRow` ships `int level = 0`, and the path example is 19 bytes not 22. **A further loop is owed** — build-changing findings were fixed this pass. |
| 0-split | 2026-08-01 | none — no reviewer dispatched | — | **Provenance row, not a review.** Split out of [ANTS-3766](ANTS-3766-roadmap-migration-archives.md) at that spec's loop 4, which stopped on the structural trigger rather than the cap: 1060 lines, and the loop's two CRITICALs plus INV-14's absence from the test plan were all loop-3-era material that two cold reads had failed to reach. This half was chosen as the seam because it is the only part with a persisted output and its own consumer. **INV-14 is inherited, not reflowed** (`specs.md` § 5.5); INV-26 and INV-27 are new and are numbered past ANTS-3756's highest so that no bare `Inv<N>` in `roadmap_store_schema` or `roadmapstore.cpp` resolves two ways (§ 3). INV-26 answers a gap the parent's loop 4 found and fixed — a column with a writer and no reader — and INV-27 makes § 2.1's no-version-bump argument falsifiable, which it was not while it lived only in prose. **The parent's four loops do NOT transfer**: they were run against a document that no longer exists, so this spec runs the rule-14 gate from loop 1 on its own bytes. |
