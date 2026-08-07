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

**Sections.** [1 Problem](#1-problem) · [2 Surface](#2-surface) —
[2.1 the column](#21-the-column), [2.2 the bump and the rung](#22-the-bump-and-the-rung),
[2.3 store API](#23-store-api), [2.4 write and read](#24-the-migration-writes-it-the-gate-consults-it),
[2.5 comparing stores](#25-comparing-a-climbed-store-with-a-ddl-built-one),
[2.6 the frozen schema](#26-the-frozen-version-1-schema) ·
[3 Invariants](#3-invariants) · [4 RAM / build cost](#4-ram--build-cost) ·
[5 Migration / compatibility](#5-migration--compatibility) ·
[6 Out of scope](#6-out-of-scope) · [7 Tests](#7-tests) ·
[8 Cross-doc impact](#8-cross-doc-impact)

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
   `GTEST_SKIP()`s below version 2 and `FAIL()`s at or above it.
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

**It is appended LAST in the `CREATE TABLE`, after `legend` — a rule, not a
layout preference.** `ALTER TABLE … ADD COLUMN` can only append, so the climbed
store's column order is fixed for it; a DDL that groups `source_format` beside
`root` instead builds a different table and reds INV-3, for a reason nothing
else in this spec would name. Every future rung inherits the same constraint.

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
to be got right now. Recorded as an alternative in § 6.

**One term, nominated once.** This spec says **format** for the stored value and
the column, matching `RoadmapMigrate::Source::format` and
`detectRoadmapFormat()`. "Dialect" appears only as its plain-English gloss (the
Layman line, and prose where "format" would read ambiguously against file
format); the two are the same thing and the column name is the canonical term.

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
QString sourceFormat;   // § 2.1. The empty string means "not recorded"; it is
                        // not a format, and no format is ever empty.
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
`setSectionSource()`, whose columns, in that rule's own words, "are nullable and
were added to a shipped signature".

**This column is a third case that rule does not name, so the argument is made
rather than borrowed:** it is `NOT NULL` *with* a default. What decides the
shape is not nullability but whether an insert that omits the column can
succeed — which is the reason ANTS-3796 gives for `position` needing a
parameter, and it is satisfied here by the `DEFAULT` exactly as it is there by
nullability. So a setter, and `''` is a meaningful value a row is entitled to
keep. Widening `registerProject()` would additionally force every
existing call site to supply a dialect, including every `registerProject()` call
in `tests/features/roadmap_store_schema/`, none of which has a roadmap at all.

### 2.4 The migration writes it, the gate consults it

**Write.** `Loader::run()` (`src/roadmapmigrateload.cpp`) already calls
`registerProject()` first; it follows that with the setter, guarded like every
other write in that function:

```cpp
if (plan.sources.isEmpty())
    return fail(QStringLiteral("migration plan has no sources"));
if (!store.setProjectSourceFormat(projectId, plan.sources.at(0).format, &err))
    return fail(err);
```

**The guard is in the block because `QVector::at()` is unchecked in a release
build** — it asserts under `QT_DEBUG` and is undefined otherwise — so a prose
rule saying "refuse rather than read past the end" that is not in the code an
implementer copies is not a rule at all.

**`MigrationPlan::sources` must carry the element-0 guarantee, and today it does
not.** `src/roadmapmigrate.h` states "element 0 is always the live roadmap" on
`RoadmapMigrate::Discovery` and documents `MigrationPlan::sources` only as
"Ordered and index-stable within a run". That guarantee is the **sole**
precondition INV-2 rests on — without it, "the stored value is the live
roadmap's format" is not implied by reading index 0. So this change adds the
same comment to `MigrationPlan::sources` (§ 8), which is a documentation fix to
an ordering the loader already preserves, not a behaviour change.

`Loader::run()` runs inside a transaction its caller opened — `store.begin()`,
`commit()` and `rollback()` all live in the outer `load()`, not in `run()` — so
the project row and its format commit together. That is the per-project
atomicity ANTS-3765 § 2.10 fixes as the dispatch marker and ANTS-3793 § 2.2
relies on; this spec states no atomicity rule of its own.

`setProjectSourceFormat()` returns false with `*error` set in two cases, and
both abort the load: an unknown `projectId` — an `UPDATE` matching no row
*succeeds* in SQLite, so the method checks `numRowsAffected()` rather than the
exec result — and a `format` outside § 2.1's `CHECK` set, which SQLite refuses.
INV-7 covers all three refusals.

**A migrated project never records `''`.** `RoadmapParse::detectRoadmapFormat()`
returns one of the three dialect names on every path, including for empty input
(`src/roadmapparse.cpp` returns `"ants-v1"` when `lines.isEmpty()`), so a plan's
`Source::format` is never the empty string. That is what keeps `''` a reliable
"pre-bump row" sentinel rather than an ambiguous value the drift detection in
the table below would silently skip.

**Read.** `RoadmapSource::migratedProject()` keeps its live
`detectRoadmapFormat()` call and gains the stored value as a second witness.
**The stored value costs no extra query** — the function already holds the
`ProjectRow` its `readProjectByRoot()` lookup returned, and § 2.3 adds the
column to that same `SELECT`. This is what § 1's consequence 1 turns on: the
comparison below is free, and only the file read (ANTS-3863) is not.
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
markdown branch silently. With two witnesses it is nameable, and ANTS-3793
INV-1's no-silent-fallback rule reaches it.

**The refusal names both formats, because `SourceUnrecognised` now carries two
causes with different remedies.** ANTS-3793 chose a distinct code for the
file-is-mangled case precisely so the store and the file "send the user to
different places"; a second cause under the same code loses that unless the
message separates them:

```
project <root>: store records format '<stored>' but its roadmap now reads as
'<detected>'
```

against the existing "has a store row but its roadmap text is unrecognisable".
**This is a behaviour change at all three callers**, on exactly one input class:
a migrated project whose live file changed dialect is now refused where it was
previously served markdown. Their code is untouched; what a user sees is not.
All three already have a `SourceUnrecognised` path, so none needs new handling —
`RemoteControl::roadmapStoreServes()` surfaces it through
`rcdetail::rcRoadmapSourceRefused()`, and the dialog and the seam through their
existing `ReadError` branches.

**The live read is not removed and no signature changes.** The gate still needs
the file's testimony for both refusal rows, and every consumer already holds the
text before the dispatch runs. The change lands in `migratedProject()` alone —
the single place a roadmap's format is classified — so the *code* of its three
callers is untouched: `RoadmapSource::bulletsFor()`, `RemoteControl::roadmapStoreServes()`
(the gate the MCP verbs go through, which does not use the seam) and
`RoadmapDialog::storeProjectRoot()`. ANTS-3863 owns moving the dispatch ahead of
the read; § 6 records the split.

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

Measured on both texts, with the real `project` DDL, on SQLite 3.53.2:

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

**The residual limitation, stated because the table above implies there is
none:** the rule deletes spaces adjacent to `,` `(` `)`, so it still normalises
away a difference inside a literal containing one of those beside a space —
`DEFAULT 'a, b'` and `DEFAULT 'a,b'` are measured to compare **equal**, a
false-equal of exactly the kind the `tr -d` row is rejected for. No such literal
exists in this schema (every default is `''`, `'{}'`, `'[]'` or `'public'`), and
introducing one is the trigger to revisit this rule rather than to widen it
quietly.

This amends ANTS-3781 INV-8 in place — see § 8. **Its normalisation
parenthetical sits inside the claim sentence, so the claim text does change**:
`(normalised for whitespace)` is replaced by a pointer to this section. The
invariant's **id** and its *Breaks when* clause are untouched, and a
`specs.md` § 5.5 annotation records the amendment — which is the distinction
that rule protects (a contract corrected in place and traceable, never
renumbered or silently reworded).

### 2.6 The frozen version-1 schema

INV-8's test needs a store *at version 1*, and a version-2 build cannot make
one: `createSchema()` carries exactly one DDL and it is the current one. So the
test carries a frozen copy — the version-1 `CREATE TABLE` / `CREATE INDEX`
statements as a `constexpr` raw-string array in
`tests/features/roadmap_store_upgrade/test_roadmap_store_upgrade.cpp`, executed
against a raw `QSqlDatabase` with `PRAGMA user_version = 1`. **The prohibition
is on the seeding only:** the frozen statements never run through
`RoadmapStore::open()`, because a version-2 build's `open()` would create the
version-2 shape. Reopening the seeded file through `open()` afterwards is not
an exception to that — it *is* the climb under test (INV-4).

Embedded rather than a committed `.sql` fixture: the bundle would need a new
compile definition to locate the file, and the frozen text is never edited again
by design, so a data file buys nothing it does not also cost.

**It is generated, not transcribed** — dumped from a store built by the
*pre-bump* binary, the same "write it from the diff, not from the shape you
intended" discipline the rung itself is under. Each future bump freezes one more.

**The dump query is exactly this, and both clauses are load-bearing:**

```sql
SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid;
```

- `WHERE sql IS NOT NULL` drops the `sqlite_autoindex_project_1` / `_2` rows
  that `root TEXT UNIQUE` and `export_slug … UNIQUE` create. Their `sql` is
  `NULL`, so without the filter the frozen array carries empty statements. They
  need no replaying — the `UNIQUE` constraints recreate them.
- `ORDER BY rowid` is creation order, which puts every table ahead of the
  indexes on it. `ORDER BY name` interleaves the two alphabetically and the
  replay fails: measured, `idx_element_item` sorts before `item`, and seeding a
  fresh database from that dump aborts with `no such table: main.item`.

## 3. Invariants

- **INV-1** — `project.source_format` exists as `TEXT NOT NULL` with default
  `''`, and its `CHECK` admits exactly `''`, `ants-v1`, `github-task-list` and
  `pass-headings`. *Test:* `roadmap_store_schema` — `PRAGMA table_info(project)`
  names the column with `notnull = 1` and `dflt_value` holding the
  **two-character SQL text** `''`, which is the default *expression* and not an
  empty string (`quote(dflt_value)` returns `''''''`); and an `UPDATE` to a
  value outside the set is refused. *Breaks when:* the column is added nullable,
  which reds the `notnull` leg, or the `CHECK` is dropped, which reds the
  `UPDATE` leg. This test builds through the DDL and never climbs, so a
  defective *rung* is INV-4's to catch and not this one's.
- **INV-2** — The migration records **source index 0's** format and no other,
  and source index 0 is the live roadmap. *Test:* `roadmap_migrate_load`, in two
  legs, because no single fixture proves both halves. **(a)** Load a two-source
  plan whose `sources[0].format` and `sources[1].format` differ, then assert
  `readProject()`'s `ProjectRow::sourceFormat` equals `sources[0].format`. That
  proves the write reads index 0 and *nothing more*, because the test builds the
  plan itself. **(b)** Run discovery over a real live-roadmap-plus-archive pair
  and assert `plan.sources.at(0).path` is the live roadmap — the half that ties
  index 0 to the live file, which leg (a) assumes and cannot check. *Breaks
  when:* the write is keyed off a plan-level or last-source format, which
  silently records an archive's grammar for the whole project (leg a); or the
  loader reorders `sources`, which leaves leg (a) green and makes the stored
  value an archive's (leg b).
- **INV-3** — A store the DDL builds at version 2 and a store the ladder climbs
  from version 1 have identical `sqlite_master` contents under § 2.5's
  normalisation, and that normalisation is § 2.5's exactly. *Test:*
  `roadmap_store_upgrade` `Inv8DdlBuiltAndClimbedStoresMatch` — the case keeps
  ANTS-3781's INV-8 name, which this spec renumbers locally but does not rename
  in the suite. Build both in one `QTemporaryDir`, compare the derived statement
  set (never a hardcoded table list), and assert the pair is **unequal** under
  plain whitespace collapsing, so the weaker rule cannot be substituted without
  reddening. That negative leg is pinned to how SQLite 3.53.x renders an ALTER'd
  table: a driver that ever re-rendered the stored SQL would make two correct
  schemas compare equal and red it, and the response then is to drop the leg,
  never to weaken the positive comparison it guards.
  *Breaks when:* the rung is written from the intended shape rather than the DDL
  diff, or the normalisation is relaxed to the one ANTS-3781 first wrote — which
  reds this invariant against a correct rung and would be "fixed" by weakening
  the comparison until it passes. It is also the only leg that sees a rung
  written as a table **rebuild** (`CREATE TABLE … AS SELECT`, drop, rename):
  such a rung copies the rows, so all four of INV-4's assertions pass while
  `root`'s `UNIQUE` and every foreign key referencing `project` have vanished
  from `sqlite_master`.
- **INV-4** — A version-1 store keeps its data across the climb. A store opened
  by a version-2 build reports `user_version = 2`, `createdSchema() == false`,
  and every project row written before the climb reads back with its original
  fields intact and `sourceFormat == ""`. *Test:* `roadmap_store_upgrade` — seed
  a store from § 2.6's frozen version-1 schema, insert a project, close, reopen
  through `RoadmapStore::open()`, assert all four. *Breaks when:* the rung omits
  its `DEFAULT`, which fails the `ALTER` outright on a store carrying a project
  row (§ 2.2) so `open()` returns false; or `m_createdSchema` is assigned on the
  upgrade arm, which reds the `createdSchema()` leg. A rung written as a table
  *rebuild* is deliberately **not** listed here — it satisfies all four of these
  assertions, and INV-3 is the leg that catches it.
- **INV-5** — A project whose `source_format` is `''` dispatches exactly as it
  did at version 1: ANTS-3793 § 2.2's four-row table decides on the live file's
  format alone, and no new refusal is reachable. *Test:* `roadmap_read_seam` — the existing dispatch cases run
  unchanged against a project row left at `''`, including the `pass-headings`
  markdown-served case and the `sawSignal == false` refusal. *Breaks when:* the
  stored value is treated as authoritative before it is known to be set, which
  turns every pre-existing migrated project into a `SourceUnrecognised`
  refusal — the empty string matching no dialect the file can produce.
- **INV-6** — A set `source_format` that disagrees with the live file's detected
  format refuses, and serves neither backend. A project stored as `ants-v1` whose
  `ROADMAP.md` now reads as `github-task-list` returns `nullopt` with
  `ReadError::SourceUnrecognised` and `*error` set. *Test:* `roadmap_read_seam`
  — migrate an `ants-v1` project, overwrite its roadmap with a GFM task list,
  assert the refusal and that no records come back. *Breaks when:* the
  disagreement falls through to the markdown backend, which is ANTS-3793 INV-1's
  silent fallback arriving through the one door that spec could not close with a
  single witness.
- **INV-7** — Every refusal on the write path aborts the load and names its
  cause. `setProjectSourceFormat()` returns false with `*error` set for an
  unknown `projectId` and for a `format` outside § 2.1's `CHECK` set, and
  `Loader::run()` refuses a plan whose `sources` is empty before it indexes
  them. *Test:* `roadmap_migrate_load` — three cases: the setter against a
  `projectId` no row carries, the setter with `"klingon"`, and a plan with no
  sources; each asserts a false return, a non-empty `*error`, and that the load
  did not commit. *Breaks when:* the setter reports success for an unknown id —
  an `UPDATE` matching no row *succeeds* in SQLite, so a bare `exec()` result is
  green — which records nothing while the load reports success. That is the
  ANTS-3767 failure mode one column along: a column with a writer that always
  claims to have written.

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

**Build.** No new target, no new library, no new external dependency — § 7 names
the suites and their existing wiring. § 2.6 adds a raw string to one test file.

## 5. Migration / compatibility

**Forward (a version-1 store meeting this build): it climbs.** `createSchema()`
routes it to `applyUpgrades()`, § 2.2's rung runs, existing project rows take
`''`, and INV-4 is the contract.

**Backward (a version-2 store meeting a PRE-BUMP binary): the store will not
open at all**, and this is the first moment in the store's life that case is
reachable. ANTS-3756 § 2.3 fixed the behaviour and the reasoning, and both stand
unchanged here: a `user_version` higher than the binary's is refused outright
rather than opened read-only, because a newer schema can move meaning and not
only add to it, so a confident partial read is worse than no read. The refusal
names both numbers.

**It is reachable because the launcher can leave an older binary on disk** —
ANTS-3756 § 2.3 names exactly this, and `CLAUDE.md` documents `launch.sh`
copying the freshest build to `~/.local/share/ants-terminal/bin/`. A user who
runs this build once and then starts an older one gets a store that binary
refuses.

**A rung that fails mid-climb leaves the store at version 1**, because
`applyUpgrades()` runs inside `createSchema()`'s transaction and stamps
`user_version` once after the last rung; the caller rolls back and `open()`
returns false. ANTS-3781 § 2.1 and its INV-7 own that path in full — it is
named here only so this section's three store/binary pairings are not read as
exhaustive of the failure space.

**The accepted answer is relaunch, and no compatibility path is owed.** The
refusal is legible and self-describing (it names the store's version and the
binary's), the condition is transient, and the store is primary — its only
rebuild route is its own export (ANTS-3761), which is a recovery path and not a
downgrade path. Building a downgrade rung would mean a rung that *drops* a
column, which discards data to satisfy a binary the user is about to replace.

## 6. Out of scope

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

## 7. Tests

All four suites are existing directories already wired into the `test_core`
bundle in `CMakeLists.txt`; no `add_executable`, per
`tests/features/README.md`'s bundle rule. Label `features;fast`.

| Directory | Covers | Change |
|---|---|---|
| `tests/features/roadmap_store_schema/` | INV-1 | New column case. **Retires the `EXPECT_EQ(RoadmapStore::kSchemaVersion, 1)` leg of `Inv27SchemaVersionStillOne`**, whose own message names this item as the change entitled to move it. The `PRAGMA user_version == kSchemaVersion` leg stays. |
| `tests/features/roadmap_migrate_load/` | INV-2, INV-7 | New two-source case, a discovery case for INV-2 leg (b), and three refusal cases. |
| `tests/features/roadmap_store_upgrade/` | INV-3, INV-4 | **Replaces `Inv8DdlBuiltAndClimbedStoresMatch`'s tripwire body** (today a `GTEST_SKIP()` below version 2 and a `FAIL()` at or above it) with the real comparison, and adds § 2.6's frozen schema. ANTS-3781's `Inv4ProductionLadderIsComplete` stops being vacuous here with no edit — its loop range becomes non-empty at the bump, which is what it was written for. |
| `tests/features/roadmap_read_seam/` | INV-5, INV-6 | New disagreement case; existing dispatch cases re-run against `''`. |

**Two tests go red the moment `kSchemaVersion` moves, by design, and closing
them is this item's work, not a surprise:** `Inv27SchemaVersionStillOne`'s
constant leg, and `Inv8DdlBuiltAndClimbedStoresMatch`'s `FAIL()`.

**`Inv27SchemaVersionStillOne` keeps its name, deliberately** — the same call
INV-3 makes for `Inv8DdlBuiltAndClimbedStoresMatch`, and for the same reason:
a test name is a handle cited from two specs' invariants (ANTS-3782 INV-27,
ANTS-3796 INV-6) and renaming it strands both. The name becomes historical, as
`Inv8…` already is; the surviving
`PRAGMA user_version == kSchemaVersion` leg is what it asserts now, and § 8's
annotations are where a reader learns why.

**Must-fail-first.** Per the project convention each new case is verified RED
before the fix is restored, and where an invariant's *Breaks when* clause names
a specific mutation the proof is that mutation, applied and reverted. **Each
mutation must be aimed at the test that can actually see it**, which is not
always the obvious one:

| Invariant | Mutation that reds it |
|---|---|
| INV-1 | drop the `DEFAULT` or the `CHECK` from § 2.1's `CREATE TABLE`. **Not** a defective rung: INV-1's test builds through the DDL and never climbs, so a rung mutation leaves it green. |
| INV-3 | the plain whitespace-collapsing normalisation § 2.5 rejects. |
| INV-4 | a rung with no `DEFAULT`, against a store carrying a project row — the case § 2.2 measures as failing the `ALTER`. This is the mutation that has to climb to be seen. |
| INV-6 | a gate that falls through to markdown on disagreement. |
| INV-7 | a setter that returns the bare `exec()` result instead of checking `numRowsAffected()` — reds the unknown-`projectId` case only, which is why that case is listed separately from the `CHECK` one. |

INV-2 and INV-5 have no single-mutation proof: INV-2's write does not exist
before this change, and INV-5 asserts *unchanged* behaviour, so its evidence is
that the existing `roadmap_read_seam` cases stay green.

## 8. Cross-doc impact

| Document | Change |
|---|---|
| `docs/specs/ANTS-3781-roadmap-store-schema-upgrade.md` | INV-8's normalisation parenthetical amended per § 2.5, annotated `INV-8 amended by ANTS-3815` (`specs.md` § 5.5 — the id, claim and *Breaks when* clause are untouched). **A pointer to `ANTS-3815 § 2.5`, never a copy of the rule text** — § 2.5 forbids substituting a weaker normalisation, and two homes for one rule is how they come to differ. § 2.1's "obligation on whoever bumps" gains a pointer to this spec as the first discharge of it. |
| `docs/specs/ANTS-3793-roadmap-consumer-cutover.md` | § 2.2's sentence *"It is read off the live file because no store column records a source format — `format` lives on `RoadmapMigrate::Source` and nowhere in `roadmapstore.h`"* becomes **false** and is replaced by a pointer to § 2.4. Its four-row outcome table gains a note that § 2.4 refines rows 1–3 for a project with a stored format. |
| `docs/specs/ANTS-3782-roadmap-section-provenance.md` | **INV-27 annotated `amended by ANTS-3815`** (`specs.md` § 5.5). Its claim — *"a store created by this build reports `PRAGMA user_version` = 1"* — is what this bump falsifies, and its stated test surface is the very `EXPECT_EQ` leg § 7 retires. The id, wording and *Breaks when* clause stay; the annotation records that the bump its *Breaks when* clause predicted has now happened. |
| `docs/specs/ANTS-3796-section-record-completeness.md` | **INV-6 annotated `amended by ANTS-3815`**, identically and for the same reason — it makes the same version-1 claim against the same test and the shipped test comment says it "rides on this test rather than duplicating it". Neither invariant is renumbered or deleted. |
| `docs/standards/roadmap-data-model.md` | § 4 gains the column: what it records, that `''` means not-recorded, and that it is the live source's dialect. |
| `src/roadmapstore.h` | `kSchemaVersion` 1 → 2; `upgradeLadder()`'s comment stops saying the ladder is empty; `ProjectRow` gains `sourceFormat`. |
| `src/roadmapsource.h` | `migratedProject()`'s comment stops saying "no store column records a source format" and states § 2.4's four-row table instead. |
| `src/roadmapmigrate.h` | `MigrationPlan::sources` gains `Discovery`'s "element 0 is always the live roadmap" comment. § 2.4 records why: it is the sole precondition INV-2 rests on, and it is currently documented only on the type the plan is built *from*. |
| `docs/specs/ANTS-3756-roadmap-store-schema.md` | § 2.3's `source_path` DDL comment says a later column is "an ALTER in a rung **rather than** an edit here". This bump does **both** — ANTS-3781 § 2.1 requires the `CREATE TABLE` edit *and* the rung as two expressions of one change — so the comment becomes false when this lands and is corrected to "as well as". ANTS-3781 § 2.1 is canonical. |
| `src/roadmapstore.cpp` | The same sentence is in the live `section.source_path` DDL comment ("an ALTER in a rung rather than an edit here") and is wrong for the same reason. **A code comment, so it is surfaced rather than edited by the spec** — it lands with the implementation. |
| `CHANGELOG.md` | One `Added` entry under `[Unreleased]`. |
| `ROADMAP.md` | ANTS-3815 flipped to in-progress at implementation start; ANTS-3863 already filed. |

**Not touched:** the three export goldens. They track
`RoadmapExport::kExportSchemaVersion`, which this bump does not move
(ANTS-3781 INV-6 is the invariant that separated the two constants), and the
format column does not enter the export record.

## Cold-eyes loop log

<!-- /cold-eyes writes one row per review loop as it closes. -->

| Loop | Date | Lanes | C/H/M/L/I | Dimensions | Outcome |
|---|---|---|---|---|---|
| 2 | 2026-08-07 | 2, cold — same shared brief, no mention of loop 1 | C 1 · H 5 · M 6 · L 8 · I 2 — verified 22, dismissed 1 | dim 4×6, dim 5×3, dim 10×3, dim 2×3, dim 15×2, dim 6×2, dim 11×2, dim 1×1, dim 7×1, dim 13×1 | **Half the batch was collateral from loop 1's own edits, including both lanes' CRITICAL — and it is the class 4a step 2 exists for: a prescription written but never executed.** (1) CRITICAL, both lanes: loop 1 added a normative code block calling `plan.sources.at(0)` directly above a paragraph promising the write "refuses with a named error rather than reading past the end". `QVector::at()` is unchecked in a release build, so the block ships exactly the UB its own next sentence forbids. The guard is now *in* the block. (2) HIGH, lane A: `MigrationPlan::sources` carries no element-0-is-the-live-roadmap guarantee — it is documented only on `Discovery` — and that guarantee is the **sole** precondition INV-2 rests on. § 8 now schedules the comment onto `MigrationPlan`. (3) HIGH, both lanes: loop 1 wrote "the invariant's id, claim and *Breaks when* clause are untouched" of ANTS-3781 INV-8 while § 8 instructed replacing its normalisation parenthetical — which sits *inside* the claim sentence. A bump author could not execute both sentences; now stated as claim-text-changes-plus-annotation. (4) HIGH, lane A: loop 1's own ANTS-3756 correction cited "§ 4's `source_path` comment"; § 4 is RAM / build cost and the comment is in § 2.3 (line 446, against § 4 starting at 760). Its other half — "§ 7's ANTS-3796 bullet" — was verified **correct** and is the run's one dismissal. (5) HIGH, lane B: INV-4's *Breaks when* named a table-rebuild rung, which satisfies all four of INV-4's assertions while dropping `root`'s `UNIQUE` and every FK — invisible to its own test. Moved to INV-3, which is the `sqlite_master` leg that actually sees it; INV-4 given a *Breaks when* its assertions can catch. (6) HIGH, lane B: § 8 omitted ANTS-3756, whose DDL comment (corrected by loop 1) says a later column is "an ALTER in a rung **rather than** an edit here" — false for a bump, which ANTS-3781 § 2.1 requires to be both. Corrected there, and the identical wording in `src/roadmapstore.cpp` surfaced as a code-side edit rather than made by this spec. (7) MEDIUM: § 2.3 paraphrased ANTS-3796's rule as "have a usable default" where it says "nullable" — this column is a third case, now argued rather than borrowed; the new refusal had no error text and no statement that it changes what three callers surface; INV-2's test could not fail if index 0 were not the live roadmap, so it is now two legs; § 2.6's "never through `open()`" contradicted INV-4's "reopen through `open()`", now scoped to seeding; and nothing said `detectRoadmapFormat()` never returns empty, which is what keeps `''` an unambiguous sentinel. New **INV-7** covers the setter's two refusals and the empty-plan guard, none of which had a test surface. **Dismissed (1):** lane A doubted § 7 of ANTS-3756 carries an ANTS-3796 bullet; it does, at line 944. Doc 517 → 625 lines; `doc_dedup` clean at 67 passages, so the growth is new contract, not restatement. |
| 1 | 2026-08-07 | 2 (general-purpose, strong model, identical byte-stable shared packet, cold) | C 1 · H 5 · M 9 · L 6 · I 1 — verified 22, dismissed 3 | dim 2×5, dim 7×5, dim 10×4, dim 6×3, dim 15×2, dim 4×2, dim 5×1, dim 11×1 | **All 21 actionable findings fixed; the run's centre of gravity was that three of this spec's own prescriptions had never been executed.** (1) CRITICAL, lane B alone: § 2.6's frozen-schema recipe, `SELECT sql FROM sqlite_master ORDER BY name`, cannot produce a runnable fixture. Verified by replaying it — `sqlite_autoindex_project_1/2` carry `sql IS NULL` so the array gains empty statements, and alphabetical order puts `idx_element_item` before `item`, so the seed aborts with `no such table: main.item`. Two of six invariants depend on that fixture. Now `WHERE sql IS NOT NULL ORDER BY rowid`, with both clauses justified. (2) HIGH ×2, both lanes independently: § 8 scheduled edits to ANTS-3781 but not to the three sibling specs this bump falsifies — ANTS-3793 § 2.2's "no store column records a source format", and **ANTS-3782 INV-27 / ANTS-3796 INV-6**, whose only stated test surface is the `EXPECT_EQ(kSchemaVersion, 1)` leg § 7 retires. Rows added with `specs.md` § 5.5 annotations. (3) HIGH, both lanes: INV-1 asserted `dflt_value = ''`; `table_info.dflt_value` holds the default *expression*, so for `DEFAULT ''` it is the two-character text `''` — measured, `quote()` returns `''''''`. As written the assertion was unsatisfiable. (4) HIGH, lane B: § 7's must-fail-first aimed the no-`DEFAULT` rung mutation at INV-1, whose test builds through the DDL and never climbs — the mutation leaves it green. Reassigned to INV-4 and the recipe rebuilt as a per-invariant table, with INV-2 and INV-5 recorded as having no single-mutation proof rather than being given a fake one. (5) HIGH, lane A: no migration/compatibility statement for the direction this bump makes reachable for the first time — a version-2 store meeting a pre-bump binary is refused outright (ANTS-3756 § 2.3), and the launcher can leave an older binary on disk. New § 5. (6) MEDIUM: § 2.1 never said the column must be appended **last**, though `ALTER` can only append and INV-3 reds otherwise; "§ 1's atomicity marker" named a marker § 1 does not contain (it is ANTS-3765 § 2.10's, via ANTS-3793 § 2.2); `setProjectSourceFormat()` had no failure contract and `plan.sources.at(0)` leaned on an element-0 guarantee stated on `Discovery` and not on `MigrationPlan`; and § 2.5's rule was presented as clean where it still false-equals `DEFAULT 'a, b'` against `'a,b'` — measured, and now stated as the residual limitation. **Deferred cold read discharged** (ANTS-3781 handed it here): ANTS-3756 § 2.3's correction paragraph credited ANTS-3782/ANTS-3796's within-version-1 columns to the "second ground" while that document's own § 4 DDL comment and § 7 bullet both give the first — corrected to name both; and that DDL comment still asserted "a bump would manufacture an upgrade case nothing implements", false since `applyUpgrades()` shipped and already updated in `src/roadmapstore.cpp`'s own copy. **Found by the blast-radius sweep, not by a lane:** ANTS-3781 § 1 still said ANTS-3756 § 2.3 "conflates" the milestones in the present tense and that § 7 "lists that sentence for correction" — the correction had already been applied, so the flag was stale in the direction of claiming a live disagreement that no longer existed. **Dismissed (3),** all lane open questions that resolved on inspection: `RoadmapStore::createdSchema()` does exist (`roadmapstore.h:104`), `roadmap-data-model.md` does have a `## 4. Fields`, and ANTS-3816 is a real bullet carrying the 1,839 figure. Doc 416 → 517 lines. |
