# ANTS-3796 — Section record completeness: document order, and the columns the export drops

**Status:** spec draft (2026-08-03).
**Kind:** implement.
**Source:** ROADMAP.md ANTS-3796 + ANTS-3797, both found 2026-08-03 while
grounding ANTS-3758's render design against the schema.
**Covers:** ANTS-3796 (no section ordering column), ANTS-3797
(`section.source_path` absent from the export).
**Blocker for:** ANTS-3758 — the render
cannot reproduce a file it cannot order, into files it cannot route.
**Blocked by:** nothing.
**Pairs with:** [ANTS-3761](ANTS-3761-roadmap-export-format.md) — its § 2.4
owns the record shape this widens and its INV-2 is the invariant that should
have caught ANTS-3797; [ANTS-3782](ANTS-3782-roadmap-section-provenance.md) —
`source_path`'s home, and the precedent for changing this schema at
`user_version` 1.

**Layman:** The roadmap database forgets what order its sections go in, and its
backup forgets which ones came from an old archived file. Both have to be fixed
before the roadmap file can be generated, or the first regeneration scrambles
it.

**One spec for two ids** because they are one surface: both widen the
`section` record, both change `writeSections()` and `rebuildProject()`, and
both regenerate the same three golden files. Two documents would have to agree
with each other forever, which `specs.md` § 2 is explicit about — the ids stay
separate on the ROADMAP, only the document merges.

---

## 1. Problem

`ANTS-3758` makes `ROADMAP.md` a generated artifact. A renderer can emit only
what the store holds, and the store is missing two things the current file
depends on. Neither is a design question; both are columns that are absent or
unreachable.

### 1.1 Sections have no order

`CREATE TABLE section` in `src/roadmapstore.cpp` declares `slug`, `title`,
`level`, `intro`, `parent_id` and `source_path`. There is no ordering column.
`RoadmapMigrate::PlannedSection` carries `firstLine` / `lastLine` for its
source span and `sourceIndex` for its file, so the *migration knows the answer
while it runs* — and discards it at commit, the same shape ANTS-3782 § 2.1
describes for `source_path`.

Sibling sections are therefore ordered only by the `section_id` surrogate, and
that fails twice:

1. `RoadmapExport::loadSections()` sorts by `(depth, slug)` — depth walked from
   `parent_id` so parents precede children — because ANTS-3761 INV-13 forbids
   emitting a surrogate. Rebuild reassigns `section_id` in that order, so
   `render(live)` and `render(rebuilt)` differ. Document order survives only
   until the first recovery from backup.
2. A section created after migration can only append. `roadmap_log
   op:create_section` places a heading precisely today (`after_section` +
   `level`); against the store it has nowhere to put that. This one needs no
   disaster to fire.

**The damage is not hypothetical, and this project's own file measures it.**
Its `##` headings, in document order:

```
$ awk '/^## /{n++; if(n<=12) print n": "$0}' ROADMAP.md
1: ## Distribution-adoption overview
2: ## Per-store publication playbook
3: ## Table of Contents
4: ## 0.5.x and 0.6.x — archived
5: ## 0.7.0 — shell integration + triggers — shipped 2026-04-15
6: ## 0.7.7 — hardening pass — shipped 2026-04-15
7: ## 0.7.12 — independent-review sweep — shipped 2026-04-19
8: ## 0.7.50–0.7.59 — indie-review sweep + companion prep — shipped 2026-04-28+
9: ## 0.7.92 — indie-review #4 + Ants MCP roadmap pass (target: 2026-05-21)
10: ## 0.7.65 — Bundle G indie-review sweep + ANTS-1118 fix-pass (target: 2026-05)
11: ## 0.7.80–0.7.84 — post-0.7.79 user-feedback rolling sweep — shipped 2026-05-10
12: ## 0.7.79 — scoped indie-review #3 on TerminalGrid + TerminalWidget — shipped
```

Neither slug order nor numeric order, deliberately — `roadmap-format.md`
§ 3.5.2 makes position priority. `0.7.92` precedes `0.7.65`; `0.7.80–0.7.84`
precedes `0.7.79`. A `(depth, slug)` re-sort files three prose sections among
the version numbers and re-sequences the entire release history.

### 1.2 `source_path` is written, read, and dropped

ANTS-3782 added the column; `RoadmapMigrateLoad` writes it via
`RoadmapStore::setSectionSource()` and `RoadmapStore::readSection()` returns it
as `SectionRow::sourcePath`. The export loses it on **both** legs:

- `RoadmapExport::writeSections()` selects
  `title, level, intro, parent_id FROM section`.
- `RoadmapExport::rebuildProject()` inserts
  `(project_id, slug, title, level, intro, parent_id)`.

```
$ grep -h '"t":"section"' tests/features/roadmap_export_roundtrip/golden/*.jsonl | head -1
{"intro":null,"level":0,"parent":null,"slug":"","t":"section","title":""}
```

So a rebuild reads every section back as the live roadmap — verbatim the
outcome the column's own DDL comment says it exists to prevent: *"without it
ANTS-3758 re-emits a rotated archive back into ROADMAP.md."*

### 1.3 The invariant written for this class cannot fire

ANTS-3761 INV-2 says "every store row, and every **non-surrogate column** of
it, survives the round-trip". Its test misses § 1.2 for two independent
reasons, and either alone is sufficient:

1. **The column-wise diff enumerates columns by hand.** The section row of
   `test_roadmap_export_roundtrip.cpp`'s table is
   `SELECT p.export_slug, s.slug, s.title, s.level, s.intro, par.slug FROM
   section s …` — a literal written before ANTS-3782 existed. An enumeration
   built from a list rather than from the schema can confirm what is listed and
   can never catch an omission.
2. **The fixture never exercises the column.** `setSectionSource` appears zero
   times in that file, so both sides are `NULL` and the leg would pass even if
   the diff covered it.

This is the defect this spec most wants to prevent recurring, because
§ 2.1's new column lands in the same hole otherwise.

---

## 2. Surface

### 2.1 The column

```sql
CREATE TABLE section (
  ...
  -- ANTS-3796. Document order among this project's sections: the sequence the
  -- render emits them in, and the only record of it. The migration plan holds
  -- it (sourceIndex, then firstLine) and is discarded at commit, exactly as
  -- ANTS-3782 § 2.1 describes for source_path.
  position    INTEGER NOT NULL,
  ...
);
```

**Project-wide, not per-parent.** One sequence over every section in the
project, assigned in document order. Parents-before-children then falls out of
the data rather than being enforced: a heading always precedes its own
subheadings in the file it was read from. A per-parent sequence would need
`UNIQUE (parent_id, position)`, and SQLite treats NULLs as distinct, so the
constraint would not bind the top-level siblings that § 1.1 shows are the ones
that reorder.

**Deliberately not `UNIQUE (project_id, position)`**, which reads like the
obvious constraint and would deadlock the re-run. `element` carries
`UNIQUE (section_id, position)` and gets away with it only because
`clearSectionElements()` lets the migration delete and rewrite the whole
sequence; sections cannot be cleared, because `element.section_id` and
`item` filing reference them. A re-run that swaps two sections' positions would
then collide mid-update with no deferred-constraint escape — SQLite offers
`DEFERRABLE INITIALLY DEFERRED` for foreign keys only. Distinctness is a
writer's obligation (INV-4) rather than a DDL constraint, and § 2.2's sort key
is total whether or not it holds.

**`NOT NULL` with no default**, so every writer states it rather than silently
taking 0. There is no store to migrate: `user_version` stays 1 (INV-6), on
ANTS-3782 § 2.1's reasoning, which this change is the last to be able to use.

### 2.2 The sort key is `(position, slug)`

Total, and deterministic under a duplicate `position`. `slug` is already
`UNIQUE (project_id, slug)`, so it is a true tie-break rather than a second
ambiguity. Stating the tie-break is the whole reason § 2.1 can leave
distinctness to a writer: a bug that assigns two sections the same position
produces a *wrong but stable* order, never an unstable one, and INV-4 catches
the bug separately.

Collation is UTF-16 code-unit order in C++, matching ANTS-3761 § 2.4's note
on why ordering is not done in `ORDER BY` — SQLite's `BINARY` is UTF-8 byte
order and the two disagree on supplementary-plane characters, which are
reachable through emoji in heading slugs.

### 2.3 Writers

```cpp
// src/roadmapstore.h — both gain the parameter; neither gains an overload.
std::optional<qint64> addSection(qint64 projectId, const QString &slug,
                                 const QString &title, int level, int position,
                                 std::optional<qint64> parentId = std::nullopt,
                                 QString *error = nullptr);

bool updateSection(qint64 sectionId, const QString &title, int level,
                   int position, std::optional<qint64> parentId,
                   QString *error = nullptr);
```

`position` is a required parameter rather than a separate setter, unlike
`setSectionIntro()` and `setSectionSource()`. Those two are setters because
their columns are nullable and were added to a shipped signature; this column
is `NOT NULL`, so an insert that omitted it could not succeed and a setter
would be unreachable. `updateSection()` carries it because
`RoadmapMigrateLoad`'s re-run path writes "only if it differs" — a section that
moved must be detected as changed, which its current `title != … || level != …
|| parentId != …` comparison cannot see.

The migration assigns positions by walking `MigrationPlan` in
`(sourceIndex, firstLine)` order — the sources are index-stable within a run
(`roadmapmigrate.h`), and `firstLine` is the heading's own line, so the pair is
document order across the live roadmap and every archive.

### 2.4 The export record gains two fields

ANTS-3761 § 2.4's per-field absent/null table is exhaustive by construction, so
it gains two rows rather than relying on a general rule:

| Field | Rule |
|---|---|
| `section.position` | **always emitted.** `NOT NULL`, so it has no absent state. |
| `section.source` | **always emitted, as `null`.** The `NULL` / `''` distinction is load-bearing (ANTS-3782 § 2.3), so it follows `section.parent` and `section.intro` rather than the omitted-when-absent group. |

Named `source` on the record, not `source_path`: the record key drops the
`_path` suffix the way `parent` drops `_id`. It does **not** collide with the
item field of the same name — they are different record types, and
`{"t":"item",…,"source":…}` is `roadmap-format.md` § 3.5's `Source:` line.

```
{"t":"section","slug":"performance-2","title":"Performance","level":3,
 "position":4,"parent":null,"intro":null,"source":null}
```

Key order in the emitted bytes is RFC 8785's, not this example's —
`JsonCanonical::serialise()` sorts, and the illustration is grouped for reading.

**The reader refuses an export whose section record lacks either field**,
matching how it already treats `provenance` (ANTS-3761 INV-2 records that
behaviour). No compatibility path: the three goldens are the only exports that
exist, they are regenerated here, and a lenient reader defaulting `position` to
0 would silently flatten document order — the precise failure this spec exists
to stop, arriving through the door built to tolerate it.

**Emission order is unchanged.** `loadSections()` keeps sorting `(depth, slug)`;
`position` rides along as data. The export's job is a byte-stable file, the
render's is document order, and conflating them would put ANTS-3761's INV-1,
INV-5 and INV-18 in scope for a change that needs none of them.

### 2.5 INV-2's column list comes from the schema

`test_roadmap_export_roundtrip.cpp`'s per-table diff replaces its literal
column lists with `PRAGMA table_info(<table>)`, minus an explicit exclusion set
(the rowid-valued columns and `project.root`, which ANTS-3761 INV-2 already
excludes by name). The join keys stay hand-written — they are identity claims,
not an inventory.

This inverts the default. Today a column added to the schema is absent from the
diff and passes; afterwards it is present and fails until someone teaches the
export about it. § 1.3 is the argument, and § 2.1's column would otherwise be
the second instance of it in this lane.

---

## 3. Invariants

- **INV-1** — Document order survives the round trip: for a fixture whose
  section document order differs from its slug order, the sequence read back
  under § 2.2's sort key after export → rebuild equals the sequence in the live
  store. *Test:* `tests/features/roadmap_export_roundtrip/` seeds a project
  whose sections are, in document order, `zeta` (level 2), `alpha` (level 3,
  child of `zeta`), `mid` (level 2) — so document order, slug order and
  `(depth, slug)` order are three different sequences — exports, rebuilds into
  a temp store, and asserts the `(position, slug)` walk matches. *Breaks when:*
  the writer omits `position` and the rebuild leaves it defaulted, which the
  slug-ordered fixture of any smaller test cannot detect.
- **INV-2** — `section.source_path` survives the round trip. *Test:* same
  fixture; one section carries `docs/roadmap/0.6.md` and one carries `NULL`,
  and after rebuild `readSection()->sourcePath` returns `docs/roadmap/0.6.md`
  and `nullopt` respectively. *Breaks when:* either leg of § 1.2 is left
  unfixed — `writeSections()` not selecting the column, or `rebuildProject()`
  not inserting it. Asserted through `readSection()` and not raw SQL, because
  ANTS-3782 INV-26 already pins the reader against SQL and repeating it here
  would test that invariant rather than this one.
- **INV-3** — INV-2's column diff fails by default on a column the export does
  not carry. *Test:* `roadmap_export_roundtrip` — a leg that adds a scratch
  column to `section` on the live store only (`ALTER TABLE`, after the export,
  before the diff) and asserts the diff reports a mismatch rather than passing.
  *Breaks when:* the column list is a literal, or `PRAGMA table_info` is read
  once and cached across the two stores. This is the invariant that makes
  § 2.5 a contract rather than a tidier way to write the same test.
- **INV-4** — The migration assigns a distinct `position` to every section of a
  project, in document order across all its sources. *Test:*
  `tests/features/roadmap_migrate_load/` — loads ANTS-3766 § 6.1's baseline
  archive fixture and asserts the positions are a permutation of
  `0 … n-1` and that every live-roadmap section precedes every section from an
  earlier-indexed archive exactly as the plan orders them. *Breaks when:* the
  loader numbers per source and restarts at 0 for each archive, which produces
  duplicate positions that § 2.2's tie-break then hides behind a plausible
  slug order — the reason distinctness is asserted here rather than left to the
  absent `UNIQUE`.
- **INV-5** — The section sort key is total: two sections sharing a `position`
  order by slug, stably, in both directions of insertion. *Test:*
  `roadmap_export_roundtrip` — insert `b-slug` then `a-slug` at the same
  position, assert the walk yields `a-slug` first; repeat with the insertion
  order reversed and assert the same result. *Breaks when:* the sort is on
  `position` alone, which is stable only by accident of the underlying
  container and reorders when the query plan changes.
- **INV-6** — This change does not move the schema version: a store created by
  this build reports `PRAGMA user_version` = 1. *Test:*
  `tests/features/roadmap_store_schema/` asserts the pragma. *Breaks when:*
  `kSchemaVersion` is bumped for these columns, which requires the upgrade path
  ANTS-3781 records as absent, against zero stores that would need one. Mirrors
  ANTS-3782 INV-27, and is the **last** change entitled to that argument: the
  freedom expires at ANTS-3758's cutover, which is the next item in this lane.
- **INV-7** — An export whose section record omits `position` or `source` is
  refused, loudly, with no partial store written. *Test:*
  `roadmap_export_roundtrip` — feed `rebuildProject()` a hand-built record
  missing each field in turn; assert `false`, a non-empty error, and that the
  section table is empty afterwards. *Breaks when:* the reader defaults a
  missing `position` to 0, which reads as defensive and silently flattens the
  document order of every project restored from an older export.

---

## 4. RAM / build cost

No new build target, no new external library, no new source file — the change
is one DDL column, two signatures, four SQL statements and a test fixture in
files that already exist (`src/roadmapstore.{h,cpp}`, `src/roadmapexport.cpp`,
`src/roadmapmigrateload.cpp`).

Memory: one `int` per section row, and roughly `len("\"position\":N,")` more
bytes per section record in the export. Both scale with the section count,
which is bounded by the number of headings an author writes and is orders of
magnitude below ANTS-3756's 250 MB store-wide history cap — the only bound in
this subsystem. No figure is quoted here because none of them changes a
decision; the shape of the growth is the whole claim.

Golden regeneration: the three files under
`tests/features/roadmap_export_roundtrip/golden/` are rewritten. Per ANTS-3761
INV-18 that is a **reviewed diff**, never a "make the test pass" step — the
expected change is exactly two new keys on each `section` record and no other
byte.

---

## 5. Out of scope

- **The render itself** — tracked by
  ANTS-3758. This spec gives it an
  orderable, routable store; it decides nothing about what the file looks like.
- **`roadmap_log op:create_section` choosing a position** — tracked by
  ANTS-3793. § 1.1's second failure is
  named here as motivation; the verb's surface is that spec's.
- **A schema-upgrade path** — tracked by ANTS-3781. INV-6 is the reason this
  change does not need one, not a claim that none is needed later.
- **Reordering sections through any UI or verb.** A permanent exclusion for
  this spec rather than deferred work: `position` is written by the migration
  and preserved by the export, and nothing in this lane has asked to mutate it
  interactively. There is no follow-up id because there is no intent to build
  it.
- **Emission order in the export.** § 2.4 keeps `(depth, slug)` deliberately; a
  permanent exclusion, because changing it would reopen ANTS-3761's INV-1,
  INV-5 and INV-18 for no gain this spec can name.

---

## 6. Tests

| Test | Covers | Label |
|---|---|---|
| `tests/features/roadmap_export_roundtrip/` | INV-1, INV-2, INV-3, INV-5, INV-7 | `features;fast` |
| `tests/features/roadmap_migrate_load/` | INV-4 | `features;fast` |
| `tests/features/roadmap_store_schema/` | INV-6 | `features;fast` |

All three directories exist; no new bundle wiring, and per
`tests/features/README.md` the sources join an existing bundle's `SOURCES`
list rather than becoming standalone executables.

**Each test is verified RED against pre-fix source before the fix is
restored** — the project convention, and the one ANTS-3761's own loop-6 row
shows earning its keep, where the mutation § 6 prescribed for INV-1 turned out
to leave it green.

Two clauses here **cannot be run yet**, so they were checked by mutation on
paper instead (`/write-spec` drafting rules): deleting the rule under test from
the model of the engine and asking whether the fixture still fails.

- INV-1's fixture fails under a `position`-less writer, and *only* under it —
  the three-section shape was chosen so that document, slug and `(depth, slug)`
  order are mutually distinct. A two-section fixture would have passed against
  a writer that emitted slug order, which is the vacuity the rule warns about.
- INV-3's fixture fails only when the diff is literal. Its scratch column is
  added **after** the export and to the live store only, so no other rule —
  not the reader's strictness (INV-7), not the golden comparison — can reject
  it first.

---

## 7. Cross-doc impact

- **[ANTS-3761](ANTS-3761-roadmap-export-format.md)** — § 2.4's absent/null
  table gains the two rows in § 2.4 above; its INV-2 gains § 2.5's
  schema-derived column list. Its emission order, INV-1, INV-5 and INV-18 are
  untouched by design. Its Status line moves off `implemented (2026-07-31)` to
  record the amendment.
- **[ANTS-3756](ANTS-3756-roadmap-store-schema.md)** — its schema section gains
  `section.position`; INV-6 here is the counterpart of its unchanged
  `user_version`.
- **[ANTS-3782](ANTS-3782-roadmap-section-provenance.md)** — INV-14 and INV-26
  stand unchanged; what changes is that the column they pin now also survives a
  rebuild. Worth a note there, because "the column is correct" and "the column
  is durable" read as the same claim and were not.
- **ANTS-3758** — unblocked by this
  spec; its § 2 can then state the render's ordering and file routing as reads
  rather than as requirements.
- **`CHANGELOG.md`** — user-invisible until ANTS-3758 lands the render, so it
  carries no entry of its own; ANTS-3758's will cover the behaviour.
- **`CLAUDE.md`** — no module-map change: no new file, no new lib.

---

## 8. Open questions

- **Should `position` be dense or sparse?** Dense (`0 … n-1`) is what INV-4
  asserts and what the migration naturally produces. Sparse (gaps of 10) would
  let a future insert land between two sections without renumbering — but
  nothing asks to insert today (§ 5), and a gap policy that no writer maintains
  decays into an arbitrary integer. Recorded rather than decided, because
  ANTS-3793's `create_section` is the first caller that would care and it can
  renumber inside its own transaction.

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-origin | 2026-08-03 | none — no reviewer dispatched | — | **Provenance row, not a review.** Not a split: both ids were filed this session while grounding ANTS-3758's § 2 against the schema, before any of that spec was drafted. ANTS-3796 was found by reading `CREATE TABLE section` and noticing no ordering column against a file whose own heading order is neither slug nor numeric; ANTS-3797 by then checking whether `source_path` — the one column ANTS-3782 added for ANTS-3758's benefit — reached the export, and finding it in neither `writeSections()` nor `rebuildProject()` nor any of the three goldens. Merged into one document under `specs.md` § 2's umbrella form because they widen the same record and regenerate the same goldens. No review has run against these bytes; loop 1 is the rule-14 gate. |
