# ANTS-3796 — Section record completeness: document order, and the columns the export drops

**Status:** implemented (2026-08-03) — cold-eyes converged at loop 2; built,
all seven invariants verified RED against their "Breaks when" mutation before
the fix was restored, three goldens regenerated as a reviewed diff. Two claims
in this document were found wrong at implementation and are corrected in place
(§ 2.5 and § 4), each marked.
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

`CREATE TABLE section` in `src/roadmapstore.cpp` declares, besides its
surrogates, `slug`, `title`, `level`, `intro`, `parent_id` and `source_path`.
There is no ordering column. `RoadmapMigrate::PlannedSection` carries
`firstLine` / `lastLine` for its source span and `sourceIndex` for its file, so
the *migration knows the answer while it runs* — and discards it at commit, the
same shape ANTS-3782 § 2.1 describes for `source_path`.

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
  -- ANTS-3796 § 2.1. Document order among this project's sections: the
  -- sequence the render emits them in, and the only record of it.
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
writer's obligation **within a run's plan** (INV-4) rather than a DDL
constraint — table-wide duplicates are permitted, because § 2.3.1 keeps a
deleted heading's row and its stale position — and § 2.2's sort key is total
whether or not distinctness holds.

**`NOT NULL` with no default**, so every writer states it rather than silently
taking 0. INV-6 states why the schema version does not move for it.

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

**The comparator has a named home**, because a sort key with no owner is one
every caller re-implements:

```cpp
// src/roadmapstore.h — a free function beside SectionRow, not a member: it
// compares two rows and touches no store state.
bool sectionOrderLess(const RoadmapStore::SectionRow &a,
                      const RoadmapStore::SectionRow &b);
```

It is **new, and not a promotion of `cmpCodeUnit()`, though that helper already
compares section slugs.** `src/roadmapexport.cpp` declares
`inline int cmpCodeUnit(const QString &a, const QString &b) { return a.compare(b); }`
and calls it on `a.slug` in **both** of `loadSections()`'s comparators, as well
as on `a.text`, `a.rawId`, `a.idFold` and several tuple members elsewhere in
that file. So the two sorts genuinely share a primitive, and this spec's
collation claim is that primitive's semantics rather than a parallel assertion.

What `sectionOrderLess()` adds is the *key*, not the comparison: it takes two
`SectionRow`s and applies `(position, slug)`, where `cmpCodeUnit()` takes two
`QString`s and knows nothing about sections. Promoting the string primitive out
of its TU would move a helper eight call sites depend on and still leave the
sort key homeless, so `cmpCodeUnit()` stays exactly where it is and
`loadSections()` is untouched (§ 2.4).

Its first callers are this spec's tests; ANTS-3758's render is the second, and
the production caller. INV-1 and INV-5 therefore test the shipped comparator
rather than one written inside the test — the difference between an invariant
and a fixture agreeing with itself.

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
would be unreachable.

#### 2.3.1 The reader, without which the column is write-only

```cpp
struct SectionRow {
    ...
    int position = 0;   // ANTS-3796 § 2.1 — document order within the project
};

// The enumerator. readSection() is a point lookup and findSection() resolves
// one slug, so neither can produce the SET a sort key applies to.
std::optional<QVector<SectionRow>> listSections(qint64 projectId,
                                                QString *error = nullptr) const;
```

`readSection()` populates the field, and `RoadmapMigrateLoad`'s re-run
comparison becomes `cur->title != … || cur->level != … || cur->position != …
|| cur->parentId != …`.

**`listSections()` is what makes § 2.2's sort key reachable at all.** The store
today exposes `findSection(projectId, slug)`, `readSection(sectionId)` and
`listItems(projectId)` — a slug resolver, a point lookup, and an enumerator for
items only. Without a section enumerator, `sectionOrderLess()` has nothing to
sort through the typed surface, and INV-1, INV-4 and INV-5 would each have to
`SELECT section_id` in raw SQL and then read every row back — which is the same
reach-past-the-reader this section exists to prevent, one level up. It is
shaped after `listItems()`, which exists for exactly this reason: *"Neither is
expressible as a point lookup however many times it is called."*

**This clause is the whole reason `updateSection()` takes the parameter**, and
an earlier draft of this spec declared the writers without it — which would
have shipped exactly the defect ANTS-3782 § 2.3 was written to name: *"A column
with no reader is write-only, and INV-14 could not observe it."* The
consequences are not cosmetic. The re-run's "written only if it differs" cannot
see a section that moved, so `Outcome::sectionsWritten` stops counting changes —
the same failure `roadmapstore.h`'s own `SectionRow` comment records as *"found
missing at implementation"* for the section-intro comparison. And INV-1, INV-4
and INV-5 would each have to reach past the typed reader into raw SQL, while
INV-2 in the same list insists on `readSection()`.

The migration assigns positions by walking `MigrationPlan` in
`(sourceIndex, firstLine)` order — the sources are index-stable within a run
and **index 0 is the live roadmap**, with the rotated archives following
(`roadmapmigrate.h`; `roadmapmigrateload.cpp` says so at the `source_path`
resolution), and `firstLine` is the heading's own line. So the pair is document
order across the live roadmap first and then every archive. The synthetic root
(`level = 0`, empty slug) sorts first within its source on an unset `firstLine`
of 0, which is where it belongs and is why INV-4 can say *every* section.

**The ordinal is computed in the loader, not carried on the plan.**
`RoadmapMigrateLoad` builds a `QHash<QString /*slug*/, int>` in a pre-pass over
`plan.sections` sorted by `(sourceIndex, firstLine)`, and each `addSection()` /
`updateSection()` call reads its `wantPosition` from that map — the same shape
as the existing `wantSource`, which is likewise derived at the call site from
`plan.sources` rather than stored on `PlannedSection`. `PlannedSection` gains
**no** field: the plan already holds everything the ordinal is a function of,
and a stored copy is a second source of truth that can disagree with the pair
it was derived from.

**Two consequences of a project-wide dense sequence, both accepted rather than
worked around.** A heading inserted mid-file renumbers every section after it,
so a re-run marks them all changed and `Outcome::sectionsWritten` counts a
renumber as a change — correct, since the rows did change, but it means the
figure is not a count of *edited* sections after an insertion. And a section
present in the store but **absent from a re-run's plan** — a heading deleted
from the source — is not removed: the loader orphans surplus *items*
(`orphaned_item`) and never issues a `DELETE FROM section`, because
`element.section_id` and item filing reference the row. Such a section keeps
its stale position, so the live sequence is a permutation of `0 … n-1` **over
the sections the plan named**, which is what INV-4 asserts and all the render
walks. A stale position that collides is ordered by § 2.2's tie-break rather
than being undefined.

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

**The rebuild importer refuses an export whose section record lacks either
field.** Not new behaviour: ANTS-3761 INV-2 records the same for `provenance` —
*"§ 2.4 emits it always and the reader therefore refuses an export missing it"*
— so an always-emitted field is already one whose absence aborts the import.
No compatibility path: the three goldens are the only exports that exist, they
are regenerated here, and a lenient importer defaulting `position` to 0 would
silently flatten document order — the precise failure this spec exists to stop,
arriving through the door built to tolerate it.

**"Importer" and "reader" are different things in this document**, and the
distinction matters in a spec whose first CRITICAL was a missing reader:
`rebuildProject()` is the *importer* that consumes an export, while
`readSection()` (§ 2.3.1) is the *reader* that returns a stored row.

**The export's `meta` schema value does not move, and that is a decision rather
than an oversight.** Each export opens with `{"name":…,"project":…,"schema":1,
"t":"meta"}`, and `rebuildProject()` aborts when that value disagrees with the
export's own record-version constant. So this change widens the record shape
without moving the discriminator, and an export written before it would clear
the `meta` gate and then be refused a few lines later for the missing fields.
That is the correct outcome and the reason no compatibility path is owed: the
refusal is specific ("line N: section record missing position") where a version
mismatch would say only that the binary is too new.

**The hand-off this paragraph made has since been answered.** When it was
written, the export's `meta` value and the store's `user_version` were **one**
constant, so bumping either to express a record change bumped both — which is
what INV-6 forbids, and why separating them was deferred to ANTS-3781.
[ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md) § 2.3 did the split:
`RoadmapExport::kExportSchemaVersion` describes the JSONL record shape, the
store's own constant describes the table shape, and they move independently. No
*value* changed, so this section's conclusion and every committed golden stand —
but the "the two are one constant" premise no longer holds, and a future
record-shape change may now move the export's number alone.

**Emission order is unchanged.** Both of `loadSections()`'s orders — its
`(depth, slug)` `emitOrder` and its plain-slug `slugOrder` — are untouched;
`position` rides along as data. The export's job is a byte-stable file, the
render's is document order, and conflating them would put ANTS-3761's INV-1,
INV-5 and INV-18 in scope for a change that needs none of them.

### 2.5 ANTS-3761 INV-2's column list comes from the schema

`test_roadmap_export_roundtrip.cpp`'s per-table diff derives its column list
from `PRAGMA table_info(<table>)` rather than from a literal. Three sets, and
the third is the one a careless reading drops:

| Set | Contents | Hand-written? |
|---|---|---|
| Derived | every column `PRAGMA table_info` reports | no — this is the point |
| Excluded | `project.root` (ANTS-3761 INV-2 excludes it by name), plus each table's **own surrogate primary key** — `project_id`, `section_id`, `element_id`, `item_pk`, `rel_id`, `history_id`, `citation_id` | yes, named |
| **Substituted** | every **foreign-key** rowid column, replaced by the stable rendering the diff compares instead | **yes, and exhaustively** |

**The substitution set is not the join keys, and conflating the two deletes
coverage.** The current literals do not merely omit foreign rowid columns; they
project a stable identity *in place of* each one — `par.slug` for
`section.parent_id`, `i.id_fold` for `element.item_pk`, `p.export_slug` for
every `project_id`, `id_fold` for `relationship.src_pk` / `dst_pk`. A rule
reading "derived, minus the rowid-valued columns" therefore stops comparing
section parentage and element→item membership altogether — narrowing the very
invariant this section exists to widen. So the substitution map is written out
per column, and **a foreign-key rowid with no entry in it fails the test**
rather than being quietly skipped; that is what keeps the map exhaustive as the
schema grows.

**The diff takes the map as a parameter**, rather than closing over a
function-static literal the way `inv2Projections()` does today. Without that,
INV-3's third leg — pass a deliberately incomplete map, assert the diff fails —
has no way to inject one, and the invariant guarding exhaustiveness would be
the one part of § 2.5 that ships untested.

**A table's own surrogate PK is excluded, not substituted, and the distinction
is load-bearing** — an earlier draft said "every rowid-valued column" is
substituted, which has no answer for `element.element_id`. That column has no
single stable rendering: the element projection identifies a row by the
composite `(s.slug, e.position)`, so demanding a one-column substitute for it
would fail INV-3 against the *correct* current projection. A rowid column
defaults to **excluded** when it is the table's own PK and to **substituted**
when it points at another table.

**Corrected at implementation (2026-08-03): the excluded list above originally
named five surrogate PKs and missed two** — `project.project_id` and, the one
that matters, `citation.citation_id`. `citation` carries a rowid PK exactly as
`element` does, and the omission is not cosmetic: `citation_id` is not a foreign
key, so the substitution rule never reaches it, and an unexcluded own-PK is
compared directly — a column ANTS-3761 § 2.3 guarantees differs after a rebuild.
The diff would have failed against a *correct* export, which is the same defect
loop 2 caught for `element.element_id` one column along.

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
  a temp store, and asserts the `listSections()` walk under
  `sectionOrderLess()` matches. *Breaks when:* `writeSections()` emits the
  column but `rebuildProject()` binds a **recomputed** value — insertion
  ordinal, or the `(depth, slug)` rank it already has in hand — instead of the
  exported one. That is the reachable failure: `NOT NULL` with no default and
  INV-7's refusal between them rule out a silently defaulted `position`, so an
  earlier draft's "the rebuild leaves it defaulted" named a state the schema
  cannot reach. A fixture whose sections are in slug order cannot detect the
  recomputation either, which is what the three-section shape is for.
- **INV-2** — `section.source_path` survives the round trip. *Test:* same
  fixture; one section carries `docs/roadmap/0.6.md` and one carries `NULL`,
  and after rebuild `readSection()->sourcePath` returns `docs/roadmap/0.6.md`
  and `nullopt` respectively. *Breaks when:* either leg of § 1.2 is left
  unfixed — `writeSections()` not selecting the column, or `rebuildProject()`
  not inserting it. Asserted through `readSection()` and not raw SQL, because
  ANTS-3782 INV-26 already pins the reader against SQL and repeating it here
  would test that invariant rather than this one.
- **INV-3** — ANTS-3761 INV-2's column diff fails by default on a column the
  export does not carry, and on a rowid-valued column missing from § 2.5's
  substitution map. *Test:* `roadmap_export_roundtrip` — read
  `PRAGMA table_info` **on both stores** and assert first that the two column
  sets are equal, treating a set difference as the failure rather than letting
  it surface as a SQL error against a projection naming a column one store
  lacks; then a leg that adds a scratch column to `section` on the live store
  only (`ALTER TABLE`, after the export, before the diff) and asserts the diff
  reports that difference; then a leg that removes one entry from the
  substitution map and asserts the diff fails rather than skipping the column.
  *Breaks when:* the column list is a literal; or `PRAGMA table_info` is read
  once and reused for both stores, which makes the set comparison vacuous; or
  an unsubstituted rowid column is skipped silently, which is § 2.5's own
  failure mode. This is the invariant that makes § 2.5 a contract rather than a
  tidier way to write the same test.
- **INV-4** — The migration assigns positions that are a permutation of
  `0 … n-1` over the sections a run's plan names, in document order across all
  its sources. *Test:* `tests/features/roadmap_migrate_load/` — loads
  ANTS-3766 § 6.1's baseline archive fixture and asserts the permutation, that
  every live-roadmap section precedes every archive section (index 0 is the
  live roadmap), and that archive sections follow one another in `sourceIndex`
  order. *Breaks when:* the loader numbers per source and restarts at 0 for
  each archive, which produces duplicate positions that § 2.2's tie-break then
  hides behind a plausible slug order — the reason density is asserted here
  rather than left to the absent `UNIQUE`. Scoped to the plan's sections
  because § 2.3.1 keeps a deleted heading's row, stale position and all; a
  whole-table permutation assertion would be false on any re-run that dropped a
  heading, which is an ordinary re-run and not an error.
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
  `kSchemaVersion` is bumped for these columns, which at the time of writing
  required an upgrade path that did not exist, against zero stores that would
  need one. Both halves have since expired —
  [ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md) built the path and a
  version-1 store now exists on disk — leaving the invariant standing on what
  remains: a bump is ANTS-3815's to make deliberately, with its rung. Mirrors
  ANTS-3782 INV-27 with **one leg deliberately dropped**: that invariant also
  asserted the three goldens still import, which cannot hold here because § 4
  regenerates them — the record shape is what changed. The argument for holding
  at 1, stated here and pointed at from § 2.1 rather than split across both: no
  store is reachable from user-facing code yet, so a bump would manufacture an
  upgrade case nothing implements in order to migrate zero stores. This is the
  **last** change entitled to it — the freedom expires at ANTS-3758's cutover.
- **INV-7** — An export whose section record omits `position` or `source` is
  refused by the **rebuild importer** (`rebuildProject()`, not the
  `readSection()` reader of § 2.3.1), loudly, with no partial store written.
  *Test:* `roadmap_export_roundtrip` — feed `rebuildProject()` a hand-built
  record missing each field in turn; assert `false`, a non-empty error, and
  that the section table is empty afterwards. The atomicity half is already
  structural rather than new: `rebuildProject()` wraps its inserts in
  `BEGIN IMMEDIATE` and `ROLLBACK`s on failure, so this invariant pins
  behaviour the code has and a future refactor could lose. *Breaks when:* the
  importer defaults a missing `position` to 0, which reads as defensive and
  silently flattens the document order of every project it restores.

---

## 4. RAM / build cost

No new build target, no new external library, no new source file. The change is
one DDL column, one new struct field, three function signatures
(`addSection()`, `updateSection()`, and the new `listSections()` +
`sectionOrderLess()`), and six SQL statements — `addSection`'s INSERT,
`updateSection`'s UPDATE, `readSection`'s SELECT, `listSections`' SELECT,
`writeSections()`'s SELECT and `rebuildProject()`'s INSERT — all in files that
already exist (`src/roadmapstore.{h,cpp}`, `src/roadmapexport.cpp`,
`src/roadmapmigrateload.cpp`).

**The signature change has several test call sites, not one**, and they are the
compile-time blast radius: `addSection()` is called from
`tests/features/roadmap_store_schema/` (four call sites),
`roadmap_store_concurrency/`, `roadmap_store_identity/` and
`roadmap_export_roundtrip/` (one local helper, which is why its own five
fixture calls cost one edit), besides two in `src/roadmapmigrateload.cpp`.
Adding `position` as a required positional parameter **before** the defaulted
`parentId` makes every one of them a compile error rather than a silent
rebinding, which is the intended failure mode.

**Corrected at implementation (2026-08-03): this said FIVE test directories and
named `roadmap_migrate_load/` among them. It is four** — `roadmap_migrate_load`
never calls `addSection()`; its only mention of the name is inside a test's
failure-message string, which is what a grep for the bare identifier picks up.
Recorded rather than silently fixed because the count was the load-bearing part
of the claim, and a blast radius counted from a grep that matched prose is the
same error class as loop 1's truncated-grep finding.

Memory: one `int` per section row, and two more keys per section record in the
export (`"position":N` and `"source":null`). Both scale with the section count,
which is bounded by the number of headings an author writes and is orders of
magnitude below ANTS-3756's 250 MB store-wide history cap — the only bound in
this subsystem. No figure is quoted here because none of them changes a
decision; the shape of the growth is the whole claim.

Golden regeneration: the three files under
`tests/features/roadmap_export_roundtrip/golden/` are rewritten. Per ANTS-3761
INV-18 that is a **reviewed diff**, never a "make the test pass" step — and the
expected diff is exactly two new keys on each existing `section` record, no
other byte.

**That expectation only holds because the new fixtures are not golden-backed.**
INV-1's three-section shape (`zeta` / `alpha` / `mid`) and INV-5's
same-position pair are built in-test, in a temp store, and compared against
each other rather than against a committed file — they exist to exercise an
ordering, which a golden cannot witness any better than an assertion can. Only
the pre-existing three projects are golden-backed. Stated because the two
readings differ by a large reviewed diff: seeding five sections into the golden
fixture would rewrite every record in `alpha.jsonl` and make INV-18's reviewed
diff unreadable in the same pass that changes the record shape.

---

## 5. Out of scope

- **The render itself** — tracked by
  ANTS-3758. This spec gives it an
  orderable, routable store; it decides nothing about what the file looks like.
- **`roadmap_log op:create_section` choosing a position** — tracked by
  ANTS-3793. § 1.1's second failure is
  named here as motivation; the verb's surface is that spec's.
- **A schema-upgrade path** — [ANTS-3781](ANTS-3781-roadmap-store-schema-upgrade.md),
  which has since built one. INV-6 is the reason this change did not need it,
  not a claim that none was needed later.
- **User-initiated reordering — dragging a section, or a verb whose purpose is
  to move one.** A permanent exclusion rather than deferred work: nothing in
  this lane has asked for it, so there is no follow-up id. It does **not**
  exclude the renumbering that follows *incidentally* from inserting a heading,
  which is ANTS-3793's `create_section` and is discussed in § 2.3.1 and § 8 —
  an earlier draft's blanket "through any UI or verb" excluded that too and
  contradicted both.
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

**Only INV-6 is runnable today.** Every other clause depends on surface this
spec creates — the column, `listSections()`, `sectionOrderLess()`, the
schema-derived diff — so there is nothing to invoke, and the two-run rule
cannot fire. Each was checked by mutation on paper instead (`/write-spec`
drafting rules): delete the rule under test from the model of the engine and
ask whether the fixture still fails. Two are worth recording, because they are
the ones where the answer was not obvious.

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
  `section.position`; its `SectionRow` / `readSection()` surface gains the
  field (§ 2.3.1) and its `addSection()` / `updateSection()` signatures gain
  the parameter (§ 2.3). INV-6 here is the counterpart of its unchanged
  `user_version`. The new `sectionOrderLess()` (§ 2.2) lands beside
  `SectionRow` in `roadmapstore.h`, so that spec owns its home even though this
  one owns the sort key.
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

- ~~**Should `position` be dense or sparse?**~~ **Decided dense** (`0 … n-1`),
  which is what INV-4 asserts and what the migration naturally produces. Sparse
  (gaps of 10) would let an insert land between two sections without
  renumbering, but a gap policy no writer maintains decays into an arbitrary
  integer, and § 2.3.1 shows the renumbering is affordable — it costs a re-run
  marking the tail changed, inside a transaction that was rewriting them
  anyway. Recorded as decided rather than left open because INV-4 already
  makes it a contract, and a question a normative invariant has answered is a
  contradiction, not a question. **Revisit at ANTS-3793** if `create_section`
  finds the renumbering cost real.

---

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-origin | 2026-08-03 | none — no reviewer dispatched | — | **Provenance row, not a review.** Not a split: both ids were filed this session while grounding ANTS-3758's § 2 against the schema, before any of that spec was drafted. ANTS-3796 was found by reading `CREATE TABLE section` and noticing no ordering column against a file whose own heading order is neither slug nor numeric; ANTS-3797 by then checking whether `source_path` — the one column ANTS-3782 added for ANTS-3758's benefit — reached the export, and finding it in neither `writeSections()` nor `rebuildProject()` nor any of the three goldens. Merged into one document under `specs.md` § 2's umbrella form because they widen the same record and regenerate the same goldens. No review has run against these bytes; loop 1 is the rule-14 gate. |
| 1 | 2026-08-03 | 3 (cold, identical shared brief) | 2 / 5 / 6 / 6 / 0 | First gate. Both CRITICALs were unanimous across all three lanes, and both were defects an author's re-read could not reach. **The spec added `position` writers and no reader** — `SectionRow` / `readSection()` were never widened, so its own § 2.3 re-run rule (`written only if it differs`) was uncomputable through the surface it declared and INV-1, INV-4 and INV-5 had no typed path. That is precisely the write-only-column defect ANTS-3782 § 2.3 exists to name, reproduced one column along by the spec citing it; § 2.3.1 now fixes it. **§ 2.5's schema-derived column list deleted coverage while claiming to add it** — the diff's literals do not merely omit rowid columns, they *substitute* stable renderings for them (`par.slug` for `parent_id`, `i.id_fold` for `item_pk`, `p.export_slug` for `project_id`), so "PRAGMA minus the rowid columns" would have stopped comparing section parentage and element membership. § 2.5 is now three sets with an exhaustive substitution map, and INV-3 fails on an unsubstituted column. Five HIGH: `INV-2` used unqualified for ANTS-3761's while this spec has its own; INV-4's "earlier-indexed archive" vacuous, since index 0 **is** the live roadmap; the `(position, slug)` comparator specified with no owner; a heading deleted from a re-run's source leaving an unremovable row with a stale position, against which INV-4's whole-table permutation was false; and § 4's "no other byte" golden diff contradicting INV-1/INV-5 seeding new fixture sections. **Three lane findings dismissed as packet gaps, not defects** — ANTS-3761 INV-2 does carry the `provenance` reader-refusal clause verbatim, `rebuildProject()` is transactional (`BEGIN IMMEDIATE` / `ROLLBACK`), and no sibling spec carries a TOC. **One collateral, caught by the 4b sweep:** the comparator fix first said `cmpCodeUnit()` would be *promoted* to a `SectionRow`-taking function, which would not have served its existing `QString` callers — `sectionOrderLess()` is new and `loadSections()` is untouched. (The replacement text then mis-stated *which* callers, on a truncated grep; loop 2 caught it.) |
| 2 | 2026-08-03 | 3 (cold, identical brief — no prior-loop findings carried) | 2 / 3 / 6 / 6 / 0 | **Mostly fix collateral, which is the finding about the run rather than the document.** Split by origin before grading: roughly eight of this loop's items landed on passages loop 1 edited, against three genuine draft defects. Two CRITICAL. **`sectionOrderLess()` had no producer** — the store exposes `findSection()`, `readSection()` and `listItems()`, so a sort key over a project's sections could not be applied through the typed surface at all, and INV-1/4/5 would have reached into raw SQL to enumerate: the § 2.3.1 defect one level up, in the section written to fix it. `listSections()` closes it. **§ 2.5's substitution set over-reached from foreign-key rowids to *every* rowid**, which has no answer for `element.element_id` — identified by the composite `(slug, position)`, not by any single stable rendering — so INV-3 would have failed against the correct current projection. The set is now split, with own-PKs excluded and named. Three HIGH: the export's `{"schema":1,"t":"meta"}` discriminator, gated on the same `kSchemaVersion` INV-6 pins, was never addressed (§ 2.4 now states why it does not move); § 2.1 and INV-6 pointed the `user_version` argument at each other and neither stated it, a circularity loop 1's own de-duplication created; and **loop 1's `cmpCodeUnit` correction was itself wrong** — all three lanes caught it. That helper *is* applied to `a.slug`, twice, inside `loadSections()`; loop 1's grep was truncated by `head -3` and the author wrote the sentence from it. The "new, not a promotion" call survives on the argument that actually holds (it compares two `SectionRow`s, not two `QString`s), and the loop-1 row above is annotated rather than rewritten. Also fixed: the ordinal's computation site was unstated, INV-3's map could not be injected, `addSection()`'s five *test* call sites were missing from § 4's blast radius, and INV-1's "Breaks when" named a defaulted `position` that `NOT NULL` and INV-7 make unreachable. |
