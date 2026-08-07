# ANTS-3815 — record the source roadmap format on `project`, and make the first schema bump

**Status:** spec draft (2026-08-07).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3815 (ANTS-3793 spec § 7, 2026-08-04 — filed while
resolving cold-eyes finding H4; scope narrowed to the column by user decision,
2026-08-07).
**Blocked by:** ANTS-3781 (the ladder), ANTS-3793 (the gate), ANTS-3855 (the one
production entry point into the migration) — all three shipped.
**Blocker for:** ANTS-3863.
**Pairs with:** ANTS-3781 — this spec supplies the first rung its ladder was
built to carry, and amends its INV-8 (§ 2.5).

**Layman:** The database now remembers which roadmap dialect each project was
written in, instead of working it out from the file every time — and if the file
later changes dialect behind the database's back, we say so instead of quietly
guessing which one to believe.

## 1. Problem

`RoadmapStore` records everything the migration read about a project *except*
what dialect it read. `RoadmapMigrate::Source` carries `format`
(`src/roadmapmigrate.h`), the loader discards the plan at commit, and no column
in `src/roadmapstore.h` survives it. Four consequences.

1. **The store's dispatch gate re-derives from the file a fact the store
   already knew.** `RoadmapSource::migratedProject()` calls
   `RoadmapParse::detectRoadmapFormat()` over the live roadmap text on every
   consumer call to a migrated project, and ANTS-3793 § 2.2 says outright why:
   "It is read off the live file because no store column records a source
   format."
2. **A project whose live file changes dialect after migration is served
   silently, not reported.** ANTS-3793 § 2.2's table row 2 sends a migrated
   project whose file reads as `github-task-list` to the markdown backend with
   no error — correct for a project that was *migrated from* GFM and still is
   GFM, and a silent wrong answer for an `ants-v1` project whose file was
   rewritten. Today those two inputs are indistinguishable, because the only
   evidence available is the file itself.
3. **`kSchemaVersion` has never moved, so ANTS-3781's ladder has never run in
   production.** `RoadmapStore::upgradeLadder()` is empty by construction at
   version 1, and ANTS-3781's INV-4 (ladder completeness) and INV-8 (a climbed
   store matches a DDL-built one) are both explicitly unrunnable until a bump.
   INV-8's test is a live tripwire — `Inv8DdlBuiltAndClimbedStoresMatch`
   `GTEST_SKIP()`s below version 2 and `FAIL()`s above it.
4. **`Inv27SchemaVersionStillOne` pins the constant at 1** in
   `tests/features/roadmap_store_schema/`, and its own message names this item
   as the change entitled to move it.

Consequence 3 is why the column cannot simply be added to the DDL the way
`section.source_path` (ANTS-3782) and `section.position` (ANTS-3796) were. Both
of those took that freedom on the stated grounds that no store was reachable
from user-facing code, and both recorded that the freedom expires. It has: a
version-1 store file exists outside a test's temporary directory, and ANTS-3855
shipped the verb that creates one.

## 2. Surface

### 2.1 The column

One column on `project`, in `RoadmapStore::createSchema()`'s DDL:

```sql
source_format TEXT NOT NULL DEFAULT ''
                 CHECK (source_format IN ('', 'ants-v1', 'github-task-list', 'pass-headings'))
```

The `CHECK` set is `RoadmapParse::detectRoadmapFormat()`'s complete range
(`src/roadmapparse.h`) plus the empty string.

**`''` means "not recorded", and it is not a format.** It is the value a
version-1 store's existing rows take when the rung runs, and it must be
reachable: a rung is SQL and nothing else (ANTS-3781 § 2.1), so no rung can
open a project's roadmap and classify it. A migrated-before-this-change project
therefore carries `''` until it is re-migrated, and § 2.4 makes that the
"behave exactly as version 1 did" path rather than an error.

**Per project, not per source.** `RoadmapMigrate::Source::format` is per *file*
— deliberately, so an archive is never parsed under the live file's grammar —
and this column records only source index 0, the live roadmap. That is the only
one the gate asks about: `migratedProject()` classifies the caller's live
`markdown` and nothing else. A per-section format column would be the general
shape, and it is not built here because nothing reads it; the ladder this item
lands makes it an `ALTER` in a later rung rather than a schema decision that has
to be got right now. Recorded as an alternative in § 5.

**Not a collision.** `source_format` already appears as a JSON envelope key in
the audit lane (`src/remotecontrol_state.cpp`, specified by ANTS-1459 § LAS-3).
Different subsystem, different namespace, no shared code; the name is right in
both places and reads consistently with `section.source_path` here.

### 2.2 The bump and the rung

`RoadmapStore::kSchemaVersion` moves `1` → `2`, and `upgradeLadder()` gains its
first entry:

```cpp
const QVector<Upgrade> &RoadmapStore::upgradeLadder() {
    static const QVector<Upgrade> ladder = {
        // ANTS-3815 — the first rung. Written FROM the diff § 2.1's DDL edit
        // made, not from the shape intended (ANTS-3781 § 2.1's obligation on
        // whoever bumps); INV-3 below is what proves the two agree.
        Upgrade{2, {QStringLiteral(
            "ALTER TABLE project ADD COLUMN source_format TEXT NOT NULL DEFAULT '' "
            "CHECK (source_format IN ('', 'ants-v1', 'github-task-list', 'pass-headings'))")}},
    };
    return ladder;
}
```

Nothing else changes in `applyUpgrades()` or `createSchema()` — the version-2
build already routes a version-1 store to the ladder through `createSchema()`'s
`version != 0` arm.

**`DEFAULT ''` is load-bearing, not decoration.** SQLite refuses
`ALTER TABLE … ADD COLUMN … NOT NULL` with no default *on a table that has
rows*, and — measured on 3.53.2 — accepts it on an empty one:

```
$ sqlite3 g.db "CREATE TABLE t (a INTEGER); INSERT INTO t VALUES (1);"
$ sqlite3 g.db "ALTER TABLE t ADD COLUMN b TEXT NOT NULL;"
Error in 2nd command line argument: Cannot add a NOT NULL column with default value NULL
$ sqlite3 f.db "CREATE TABLE t (a INTEGER);"
$ sqlite3 f.db "ALTER TABLE t ADD COLUMN b TEXT NOT NULL;"   # exit 0 — column added
```

A defaultless rung would therefore pass against every empty test store and fail
against every real one, which is the worst available shape for a migration. The
default removes the question rather than relying on which side of it the driver's
bundled SQLite falls.

### 2.3 Store API

`ProjectRow` gains the field, and `readProjectWhere()`'s `SELECT` in
`src/roadmapstore.cpp` gains the column — one helper, so all three of
`readProject()` / `readProjectBySlug()` / `readProjectByRoot()` return it:

```cpp
// src/roadmapstore.h, on ProjectRow
QString sourceFormat;   // '' = not recorded (§ 2.1); never a dialect name by default
```

A **setter**, not a `registerProject()` parameter:

```cpp
bool setProjectSourceFormat(qint64 projectId, const QString &format,
                            QString *error = nullptr);
```

This follows ANTS-3796 § 2.3's stated rule rather than contradicting it. That
rule made `addSection()`'s `position` a required parameter *because* it is
`NOT NULL with no default`, so an insert omitting it could not succeed and a
setter would be unreachable — and contrasted it with `setSectionIntro()` /
`setSectionSource()`, whose columns were added to a shipped signature and have a
usable default. This column is the second kind: `NOT NULL DEFAULT ''`, added to
a shipped `registerProject()` signature, and `''` is a meaningful value a row is
entitled to keep. Widening `registerProject()` would additionally force every
existing call site to supply a dialect, including the three test call sites in
`tests/features/roadmap_store_schema/` that have no roadmap at all.

### 2.4 The migration writes it, the gate consults it

**Write.** `Loader::run()` (`src/roadmapmigrateload.cpp`) already calls
`registerProject()` first; it follows that with
`setProjectSourceFormat(projectId, plan.sources.at(0).format)`. Source index 0
is always the live roadmap (`RoadmapMigrate::Discovery`), inside the same
transaction as the rest of the load, so a project row and its format commit
together exactly as § 1's atomicity marker requires.

**Read.** `RoadmapSource::migratedProject()` keeps its live
`detectRoadmapFormat()` call and gains the stored value as a second witness.
The table below refines ANTS-3793 § 2.2's rows for a project that HAS a store
row — that spec's fourth row, "no store row → markdown", never reaches the
format check and is unchanged. Exactly one outcome is new:

| `source_format` | live `detectRoadmapFormat()` | Outcome |
|---|---|---|
| `''` | any | **exactly as version 1** — ANTS-3793 § 2.2's table decides, unchanged |
| set | `sawSignal` **false** | **refuse**, `ReadError::SourceUnrecognised` — unchanged (ANTS-3793 row 3) |
| set, `== live` | `sawSignal` set | **as ANTS-3793** — store if `ants-v1`, markdown otherwise |
| set, `!= live` | `sawSignal` set | **refuse**, `ReadError::SourceUnrecognised` — **new** |

The new row is why this item is a correctness change rather than an
optimisation. ANTS-3793 already refuses "a file that no longer looks like what
the store says it is" — that is its own wording for row 3 — but at version 1 it
could only detect the *unrecognisable* case, because an unrecognisable file is
the only disagreement a lone witness can have with itself. A file rewritten from
`ants-v1` into valid GFM is the same class of drift and today takes the
markdown branch silently. With two witnesses it is nameable, and INV-1's
no-silent-fallback rule reaches it.

**The live read is not removed and no signature changes.** The gate still needs
the file's testimony for both refusal rows, and every consumer already holds the
text before the dispatch runs. The change lands in `migratedProject()` alone —
the single place a roadmap's dialect is classified — so its three callers are
untouched: `RoadmapSource::bulletsFor()`, `RemoteControl::roadmapStoreServes()`
(the gate the MCP verbs go through, which does not use the seam) and
`RoadmapDialog::storeProjectRoot()`. ANTS-3863 owns moving the dispatch ahead of
the read; § 5 records the split.

### 2.5 Comparing a climbed store with a DDL-built one

ANTS-3781 INV-8 requires that a store built fresh at version 2 and one climbed
from version 1 have "identical `sqlite_master` contents (normalised for
whitespace)". **That parenthetical is too weak as written, and this spec fixes
the rule** — SQLite does not re-render a table's stored SQL after
`ALTER TABLE … ADD COLUMN`; it splices the new column definition in ahead of the
closing paren, moving the comma to the *start* of the appended text:

```
DDL-built:   …  legend  TEXT NOT NULL DEFAULT '{}',
               source_format TEXT NOT NULL DEFAULT '' … )

climbed:     …  legend  TEXT NOT NULL DEFAULT '{}'
             , source_format TEXT NOT NULL DEFAULT '' … )
```

Measured on both texts, with the real `project` DDL:

| Normalisation | Equal? |
|---|---|
| none (byte-identical) | **no** |
| collapse whitespace runs to one space (`tr -s`) | **no** — a space survives before the comma |
| delete all whitespace (`tr -d`) | yes, but it mangles any literal containing a space |
| **§ 2.5's rule below** | **yes** |

The rule, and the only one this spec permits:

> Collapse every run of whitespace to a single space, then delete spaces
> immediately before or after each of `,` `(` `)`, then trim.

Verified against a `DEFAULT 'hello world'` pair that the delete-all rule
corrupts and this rule handles. Two negative controls confirm it still
discriminates: a rung adding `source_fmt` where the DDL says `source_format`,
and a rung omitting the DDL's `CHECK`, both compare unequal.

This amends ANTS-3781 INV-8 in place — see § 7. The invariant's id, claim and
*Breaks when* clause are untouched; only its normalisation parenthetical is
corrected, per `specs.md` § 5.5.

### 2.6 The frozen version-1 schema

INV-8's test needs a store *at version 1*, and a version-2 build cannot make
one: `createSchema()` carries exactly one DDL and it is the current one. So the
test carries a frozen copy — the version-1 `CREATE TABLE` / `CREATE INDEX`
statements as a `constexpr` raw-string array in
`tests/features/roadmap_store_upgrade/test_roadmap_store_upgrade.cpp`, executed
against a raw `QSqlDatabase` with `PRAGMA user_version = 1`, never through
`RoadmapStore::open()`.

Embedded rather than a committed `.sql` fixture: the bundle would need a new
compile definition to locate the file, and the frozen text is never edited again
by design, so a data file buys nothing it does not also cost.

**It is generated, not transcribed** — dumped from a store built by the
*pre-bump* binary (`SELECT sql FROM sqlite_master ORDER BY name`), which is the
same "write it from the diff, not from the shape you intended" discipline the
rung itself is under. Each future bump freezes one more.

## 3. Invariants

- **INV-1** — `project.source_format` exists as `TEXT NOT NULL` with default
  `''`, and its `CHECK` admits exactly `''`, `ants-v1`, `github-task-list` and
  `pass-headings`. *Test:* `roadmap_store_schema` — `PRAGMA table_info(project)`
  names the column with `notnull = 1` and `dflt_value = ''`, and an `UPDATE` to
  a value outside the set is refused. *Breaks when:* the column is added
  nullable, or without the default, which § 2.2 shows fails the `ALTER` on any
  store that has a project row.
- **INV-2** — The migration records the **live** roadmap's dialect and no
  other. For a project whose live roadmap is `ants-v1` and whose rotated archive
  is a different dialect, the stored value is `ants-v1`. *Test:*
  `roadmap_migrate_load` — load a two-source plan whose `sources[0].format` and
  `sources[1].format` differ, then assert `readProject()` returns
  `sources[0].format`. *Breaks when:* the write is keyed off a plan-level or
  last-source format, which silently records an archive's grammar for the whole
  project.
- **INV-3** — A store the DDL builds at version 2 and a store the ladder climbs
  from version 1 have identical `sqlite_master` contents under § 2.5's
  normalisation, and that normalisation is § 2.5's exactly. *Test:*
  `roadmap_store_upgrade` `Inv8DdlBuiltAndClimbedStoresMatch` — build both in
  one `QTemporaryDir`, compare the derived statement set (never a hardcoded
  table list), and assert the pair is **unequal** under plain whitespace
  collapsing, so the weaker rule cannot be substituted without reddening.
  *Breaks when:* the rung is written from the intended shape rather than the DDL
  diff, or the normalisation is relaxed to the one ANTS-3781 first wrote — which
  reds this invariant against a correct rung and would be "fixed" by weakening
  the comparison until it passes.
- **INV-4** — A version-1 store keeps its data across the climb. A store opened
  by a version-2 build reports `user_version = 2`, `createdSchema() == false`,
  and every project row written before the climb reads back with its original
  fields intact and `sourceFormat == ""`. *Test:* `roadmap_store_upgrade` — seed
  a store from § 2.6's frozen version-1 schema, insert a project, close, reopen
  through `RoadmapStore::open()`, assert all four. *Breaks when:* the rung is
  written as a table rebuild (`CREATE TABLE … AS SELECT`, drop, rename) rather
  than an `ALTER`, which loses `root`'s `UNIQUE` and every foreign key
  referencing `project`.
- **INV-5** — A project whose `source_format` is `''` dispatches exactly as it
  did at version 1: ANTS-3793 § 2.2's four-row table decides, and no new refusal
  is reachable. *Test:* `roadmap_read_seam` — the existing dispatch cases run
  unchanged against a project row left at `''`, including the `pass-headings`
  markdown-served case and the `sawSignal == false` refusal. *Breaks when:* the
  stored value is treated as authoritative before it is known to be set, which
  turns every pre-existing migrated project into a `SourceUnrecognised`
  refusal — the empty string matching no dialect the file can produce.
- **INV-6** — A set `source_format` that disagrees with the live file's dialect
  refuses, and serves neither backend. A project stored as `ants-v1` whose
  `ROADMAP.md` now reads as `github-task-list` returns `nullopt` with
  `ReadError::SourceUnrecognised` and `*error` set. *Test:* `roadmap_read_seam`
  — migrate an `ants-v1` project, overwrite its roadmap with a GFM task list,
  assert the refusal and that no records come back. *Breaks when:* the
  disagreement falls through to the markdown backend, which is ANTS-3793 INV-1's
  silent fallback arriving through the one door that spec could not close with a
  single witness.

## 4. RAM / build cost

**The column.** One `TEXT` per project row, holding at most 16 bytes
(`github-task-list`) and usually 7 (`ants-v1`). The `project` table is one row per project
— this installation's store holds one — so the total is bytes, and no eviction
policy is owed.

**The rung.** `ALTER TABLE … ADD COLUMN` is a metadata-only operation in SQLite:
it rewrites the stored schema text and touches no row. ANTS-3781 § 4 sets the
budget at 1000 ms, a fifth of `kBusyTimeoutMs`, because the climb holds the
write lock inside `createSchema()`'s `BEGIN IMMEDIATE`. Measured against a
`project` table seeded to 1,839 rows — a deliberate overestimate: 1,839 is
ANTS-3816's measured *item* count for this project, and `project` holds one row
per project, not one per item:

```
$ sqlite3 big.db "WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i<1839)
                  INSERT INTO project (root,name,export_slug) SELECT '/r'||i,'N'||i,'p'||i FROM s;"
$ /usr/bin/time -f "%e s" sqlite3 big.db "ALTER TABLE project ADD COLUMN source_format …"
0.00 s
```

**Build.** No new target, no new library, no new external dependency. All four
test files already compile into the `test_core` bundle (`CMakeLists.txt`); § 2.6
adds a raw string to one of them.

## 5. Out of scope

- **Removing the `ROADMAP.md` read on a migrated project** — deferred, tracked
  by **ANTS-3863**. Every consumer takes the roadmap text by value and so reads
  the file before any dispatch runs; removing the read means moving the dispatch
  ahead of it, which is a signature change across `bulletsFor()`'s two call sites
  plus `RemoteControl::roadmapStoreServes()`, and a decision about the
  `sawSignal` guard. Split from this item by user decision, 2026-08-07, after two
  measurements taken that day: `ROADMAP.md` is 3.12 MiB on this project
  (`stat -c%s ROADMAP.md` → 3268674), and the scan this column replaces was
  already bounded to 300 non-blank lines by `RoadmapSource::detectionPrefix()`
  (`kDetectorLineCap`, `src/roadmapsource.cpp`). This item's headline was
  corrected in the same pass to stop claiming that saving.
- **A per-source (or per-section) format column** — a permanent exclusion *for
  this item*, not a promise. § 2.1 records why the project-level value is the
  one the gate asks for; a later consumer that must re-emit a rotated archive in
  its original dialect would want the per-source form, and the ladder this item
  lands is exactly what makes that a rung rather than a schema migration crisis.
  No id is filed, because nothing needs it yet and filing one would book work
  nobody intends to do.
- **Back-filling `''` rows** — permanent exclusion. A rung is SQL and cannot
  classify a roadmap (ANTS-3781 § 2.1); re-running the migration is the existing
  route and § 2.4 makes `''` a first-class value rather than a defect to sweep.
- **The fidelity of § 2.6's frozen version-1 schema is not independently
  checked**, and this is stated rather than papered over. INV-3 polices it
  indirectly — a frozen schema that misdescribes version 1 produces a climbed
  store that differs from the DDL-built one — but it cannot catch an error the
  rung happens to reproduce. Generating the text from a pre-bump binary rather
  than transcribing it is the whole of the mitigation.

## 6. Tests

All four suites are existing directories already wired into the `test_core`
bundle in `CMakeLists.txt`; no `add_executable`, per
`tests/features/README.md`'s bundle rule. Label `features;fast`.

| Directory | Covers | Change |
|---|---|---|
| `tests/features/roadmap_store_schema/` | INV-1 | New column case. **Retires the `EXPECT_EQ(RoadmapStore::kSchemaVersion, 1)` leg of `Inv27SchemaVersionStillOne`**, whose own message names this item as the change entitled to move it. The `PRAGMA user_version == kSchemaVersion` leg stays. |
| `tests/features/roadmap_migrate_load/` | INV-2 | New two-source case. |
| `tests/features/roadmap_store_upgrade/` | INV-3, INV-4 | **Replaces `Inv8DdlBuiltAndClimbedStoresMatch`'s tripwire body** (today a `GTEST_SKIP()` below version 2 and a `FAIL()` above it) with the real comparison, and adds § 2.6's frozen schema. ANTS-3781's `Inv4ProductionLadderIsComplete` stops being vacuous here with no edit — its loop range becomes non-empty at the bump, which is what it was written for. |
| `tests/features/roadmap_read_seam/` | INV-5, INV-6 | New disagreement case; existing dispatch cases re-run against `''`. |

**Two tests go red the moment `kSchemaVersion` moves, by design, and closing
them is this item's work, not a surprise:** `Inv27SchemaVersionStillOne`'s
constant leg, and `Inv8DdlBuiltAndClimbedStoresMatch`'s `FAIL()`.

**Must-fail-first.** Per the project convention each new case is verified RED
before the fix is restored, and for the three invariants whose *Breaks when*
clause names a specific mutation the proof is that mutation, applied and
reverted: INV-1 against a rung with no `DEFAULT` (on a store carrying a project
row), INV-3 against the plain whitespace-collapsing normalisation, and INV-6
against a gate that falls through to markdown on disagreement. INV-4's is the
climb itself — the case cannot exist before the bump.

## 7. Cross-doc impact

| Document | Change |
|---|---|
| `docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md` | INV-8's normalisation parenthetical amended per § 2.5, annotated `INV-8 amended by ANTS-3815` (`specs.md` § 5.5 — the id, claim and *Breaks when* clause are untouched). § 2.1's "obligation on whoever bumps" gains a pointer to this spec as the first discharge of it. |
| `docs/standards/roadmap-data-model.md` | § 4 gains the column: what it records, that `''` means not-recorded, and that it is the live source's dialect. |
| `src/roadmapstore.h` | `kSchemaVersion` 1 → 2; `upgradeLadder()`'s comment stops saying the ladder is empty; `ProjectRow` gains `sourceFormat`. |
| `src/roadmapsource.h` | `migratedProject()`'s comment stops saying "no store column records a source format" and states § 2.4's four-row table instead. |
| `CHANGELOG.md` | One `Added` entry under `[Unreleased]`. |
| `ROADMAP.md` | ANTS-3815 flipped to in-progress at implementation start; ANTS-3863 already filed. |

**Not touched:** the three export goldens. They track
`RoadmapExport::kExportSchemaVersion`, which this bump does not move
(ANTS-3781 INV-6 is the invariant that separated the two constants), and the
format column does not enter the export record.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Outcome |
|---|---|---|---|---|
