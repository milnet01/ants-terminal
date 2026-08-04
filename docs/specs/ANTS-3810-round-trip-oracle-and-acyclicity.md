# ANTS-3810 — the round-trip oracle, and whole-store relationship acyclicity

**Status:** draft (2026-08-04) — the rule-14 cold-eyes gate has not run. The
loop log below carries only a `0-split` provenance row, which is the signal.
**Kind:** test.
**Source:** ROADMAP.md ANTS-3810 (ANTS-3793 cold-eyes loop-3 split, 2026-08-03).
Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`,
which the user cut four ways after its cold-eyes run stopped at the loop cap.
This part carries the umbrella's §§ 2.6–2.7 and its INV-7 / INV-8. See the loop
log's `0-split` row.
**Covers:** ANTS-3810 only.
**Blocked by:** ANTS-3758 (the render this oracle drives) — shipped. ANTS-3765
(the migration loader) — shipped. ANTS-3761 (the export) — shipped. Nothing in
§ 2 needs ANTS-3793, ANTS-3808 or ANTS-3809 to have landed; § 2.1.3 states why
the "build it before the ANTS-3808 fix" scheduling constraint the umbrella
asserted is not one.
**Blocker for:** ANTS-3794 (publish + health checks), which schedules the check
§ 2.2 declares and inherits the family it opens.
**Pairs with:** ANTS-3809 (the write half). Its § 2.1 commits the store and then
publishes with a real render; this oracle is what proves that publish is
lossless.

**Why one document holds both halves.** They are the umbrella's two leftovers,
and the cohesion is real but modest: each is a **check over a finished store
that reports rather than refuses**, neither has a production caller, and both
are the first members of the health-check family ANTS-3794 schedules. That is
the whole of the relation, and it is stated rather than dressed up — the
umbrella's own loop-2 tail recorded that co-locating a graph check with a
*reader seam* was cohesion invented after the fact, and § 2.2 does not repeat
that mistake.

**Contents:** [1. Problem](#1-problem) · [2. Surface](#2-surface)
([2.1 The oracle](#21-the-oracle-render-rediscover-reload-compare) ·
[2.1.1 What the projection excludes](#211-what-the-projection-excludes-and-three-fields--26-misses) ·
[2.1.2 Non-vacuity](#212-non-vacuity-the-gate-the-fixture-and-the-red-proof) ·
[2.2 Acyclicity](#22-whole-store-relationship-acyclicity)) ·
[3. Invariants](#3-invariants) ·
[4. RAM, latency and build cost](#4-ram-latency-and-build-cost) ·
[5. Out of scope](#5-out-of-scope) · [6. Tests](#6-tests) ·
[7. Cross-doc impact](#7-cross-doc-impact)

## 1. Problem

**Two claims in the roadmap-store family are asserted by a contract and proved
by nothing.**

**The first is ANTS-3758's INV-1.** It states the full round trip — render a
store, re-load the render from disk into a scratch store, export both, compare —
and names a shipped test, `Inv1ExportsMatch`. That case does not do it. It
renders once and asserts `text.contains(...)` for five field values
(`tests/features/roadmap_render/test_roadmap_render.cpp`, the case's own comment
says so: *"the full render → load → export comparison is what ANTS-3793's
cutover work wires up"*). What it proves is the half that stands alone — the
fields reach the rendered text — because the full comparison needs
`RoadmapMigrateLoad::load()`, a second store and its `Options`, none of which
that spec had in hand. So the strongest claim about the render is carried by a
case that cannot reach it.

**That gap is not theoretical, and ANTS-3808 is the proof.** The migration
stores the whole bullet into `item.body` while `renderBullet()` reads that
column as residual prose, so every rendered bullet repeats its own headline and
every trailer key. A working oracle fails on that immediately. It shipped
undetected because the only thing watching the render was a `contains()` check,
which a duplicated bullet passes.

**The second is whole-store relationship acyclicity.** `roadmap-data-model.md`
§ 6 declares `splits-from`, `blocked-by`, `duplicate-of` and `supersedes`
acyclic, and says the property is checked over the full store only. The schema
enforces `CHECK (dst_pk IS NULL OR dst_pk <> src_pk)`
(`src/roadmapstore.cpp`, the `relationship` DDL) — which stops `A → A` and
cannot see `A → B → A`. ANTS-3756 § 5 records the graph walk as belonging to
another id and files it out. Nothing has built it. Deferred here by ANTS-3760
finding 9.

## 2. Surface

### 2.1 The oracle: render, rediscover, reload, compare

**This spec builds ANTS-3758 § 2.6's contract and adds nothing to it**, except
the three exclusions § 2.1.1 shows are missing from its enumeration. The
pipeline, with every symbol resolved:

```
  RoadmapRender::render(src, projectId, scratchRoot, opts, &err)
        │                                    ─▶ files under scratchRoot
        │                                              │
        │                     RoadmapMigrate::findRoadmaps(scratchRoot, &err)
        │                        ─▶ RoadmapMigrate::planFrom(disc, name, slug)
        │                           ─▶ RoadmapMigrateLoad::load(dst, plan, o)
        │                                                        │
  src ──RoadmapExport::writeProject──▶ A ─┐        B ◀───────────┘
                                          └── projection, then compare ──┘
```

**The render is written to a scratch project root, with archives at their
`source_path`s.** `findRoadmaps()` discovers the file set from disk, so the test
has to reproduce the layout rather than hand the loader one string. The
`section` export record carries `source` (`src/roadmapexport.cpp`, the section
emitter), which is the field that layout proves.

**Four call-shape rules the pipeline imposes, each from the code and none
optional.**

1. **The scratch store opens on `Access::Bulk`.** `RoadmapMigrateLoad::load()`
   refuses an `Interactive` connection rather than running slowly (its header's
   own comment, ANTS-3765 INV-12). `Access` is the **third** constructor
   parameter, after `historyCapBytes`.
2. **`Options::changedAt` is supplied, not read from a clock.** `history.changed_at`
   CHECKs a full ISO-8601 Z timestamp and a malformed value refuses the whole
   load with a `bad_options` note before the transaction opens.
3. **`planFrom()` is given the source project's own `projectName` and
   `exportSlug`.** `RoadmapExport::writeProject()` addresses a project by slug,
   and the `meta` record carries `project` (the slug) and `name`. A scratch
   project registered under a different slug would make side B unaddressable
   and side A's `meta` unmatchable.
4. **The scratch root does not leak into the comparison.** Verified rather than
   assumed: the `meta` record emits `schema`, `project` and `name` and **no
   root** (`src/roadmapexport.cpp`, the meta emitter), and `registerProject()`
   canonicalises the root into a column the export never reads. So the two
   sides differing in project root is not a difference the exports can show.

**The projection predicate is a test helper in this feature's own directory,
not production code.** Nothing in the product compares two exports; the oracle
exists to hold the render honest. The helper is written once and applied to
**both** sides, which is § 2.6's rule and the reason it is a rule: families 2
and 3 are present in B as well, since the re-load writes its own history and
reconstructs its own `id_origin` and `provenance`.

#### 2.1.1 What the projection excludes, and three fields § 2.6 misses

ANTS-3758 § 2.6 enumerates three families and is cited rather than restated:
(1) items the render excludes by design — `visibility = 'internal'` and
`status = 'dropped'`; (2) record kinds markdown does not carry — `history`,
`relationship`, `citation`, `feedback_ref` and the `id_prefix` high-water;
(3) per-item fields the export emits and markdown has no carrier for —
`id_origin`, `provenance`, `created` / `last_modified` / `shipped`, and
`milestone`.

**Family 3's enumeration is incomplete, and the oracle cannot be built without
completing it.** Three more per-item fields satisfy family 3's own definition —
the export emits them and markdown has no carrier — and each was verified
against source rather than reasoned about:

| Field | Export | Render | Re-load |
|---|---|---|---|
| `resolution` | emitted (`insertIfPresent`) | **no carrier** — `grep -n -i resolution src/roadmaprender.cpp` returns nothing, and `grep -ni resolution docs/standards/roadmap-format.md` defines no bullet line | excluded by `fieldsOf()` (`src/roadmapmigrateload.cpp`), whose comment names it |
| `priority` | emitted when non-NULL | **no carrier** — `grep -ci priority src/roadmaprender.cpp` returns 0, and `roadmap-format.md` encodes priority as bullet *position*, not as a value | excluded by the same `fieldsOf()` comment |
| `extras` | emitted (one of the four JSON columns) | **no carrier** — `grep -n extras src/roadmaprender.cpp` returns nothing | *written* by `fieldsOf()`, but from keys the render cannot reproduce — below |

**`extras` is the one that needs its reasoning shown, because it is the one
that looks like it should round-trip.** Unlike `id_origin` and `provenance`, the
migration does write `extras`, and it derives its keys from the source text —
so a first reading says the re-load reconstructs them and the column survives.
It does not. The two keys the migration generates hold what normalisation
*discarded*: `source_kind` is the raw `Kind:` token when it was mapped or
unmapped (`src/roadmapmigrate.cpp`, the Kind branch), and `source_status` is
the raw pass-format status word. The render emits the **canonical** value —
`Kind: fix`, never the `bug` it was mapped from — so the raw token is
unrecoverable from the rendered text by construction. `roadmapmigrate.cpp`'s
own comment says as much about `source_status`: *"storing the normalised word
would lose `completed` vs `done`, which the write-back being a right-inverse
makes unrecoverable."* Export A carries `{"source_kind":"bug"}`, export B
carries `{}`, and they differ for a reason that is the format's, not the
render's. Family 3 is where it belongs.

**Consequence for ANTS-3758's INV-1, and it is not cosmetic.** That invariant's
*Breaks when* clause reads *"a non-defaultable field — `layman`, `body`,
`resolution`, `lanes`, `evidence`, an `extras` key — is dropped from the
bullet"*. Two of the six named fields are ones the same spec's § 2.6 predicate
must exclude, so as written INV-1 is **unsatisfiable for any item carrying a
`resolution` or a migration-generated `extras` key** — which is every item whose
source `Kind:` needed mapping. § 7 amends both the clause and the enumeration.
Whether the render *should* instead grow a carrier so a closed item's rationale
survives publication is a real question and a different one; it is filed as
**ANTS-3824** and § 5 puts it out of scope.

**`visibility` needs no exclusion and that is worth stating**, because it looks
like it belongs beside `priority`. It is always emitted and the migration never
writes it, so a re-load leaves it at the column DEFAULT `'public'` — which is
the value side A holds for every item family 1 did not already remove. It
round-trips *given* family 1, and only given it.

#### 2.1.2 Non-vacuity: the gate, the fixture, and the red proof

An export comparison is the easiest kind of test to make pass by accident: two
empty projections are equal. Three rules close that off, and each answers the
question *which rule makes this fixture fail, and is it the rule under test?*

**The render's INV-5 gate is asserted explicitly, not left to fail the diff.**
`RoadmapRender::render()` returns an *engaged* `Outcome` with `gateFailures`
listing public open items that carry no `layman`, and writes nothing
(`src/roadmaprender.h`). A fixture that trips it renders no files, so
`findRoadmaps()` finds none and side B is near-empty — the comparison fails,
but it fails as an unreadable diff rather than as one line. The oracle asserts
`gateFailures.isEmpty()` and `committed` before it compares anything. Every
public open item in the fixture carries a `layman`.

**The fixture populates every field family that survives the projection**, and
the case asserts the projected record set is non-empty and contains at least one
`item`, one `section` and one `element` record. Concretely it carries: an item
with `body`, `layman`, `source`, `lanes` and `evidence` all set; a second item
in the same section whose insertion order differs from its position; a nested
section; and one archive section with a `source_path`, so the scratch layout is
exercised rather than assumed.

**The red proof is the project's mutation harness, not a scheduling
constraint.** The umbrella required this oracle to be *built before* ANTS-3808's
fix and shown red against it, on the correct grounds that a fixture only ever
run against corrected code proves the oracle compiles rather than that it
discriminates. The constraint is unnecessary: `testing.md`'s convention already
requires every case to be verified RED against its *Breaks when* mutation before
the implementation is restored, and for INV-1 that mutation is *"restore the
pre-ANTS-3808 `item.body` write"* — the whole bullet into `body`, which is what
`src/roadmapmigrate.cpp` does today. The proof is available whether ANTS-3808
has landed or not, so this spec declares the mutation and imposes no ordering
between the two ids. (Ordering that has to hold across two ids is ordering
someone will get wrong once and never notice.)

### 2.2 Whole-store relationship acyclicity

**It is a check, not a constraint.** SQLite cannot express graph reachability in
DDL, and enforcing acyclicity inside `relateItems()` would put a traversal in
the write path of the migration's hottest loop. It runs over a finished store
and **reports** rather than refuses — a cycle is a data fault to surface, not a
write to reject after the fact.

**Three corrections to the shape the umbrella sketched**, each because the
sketch contradicts a document or a column it inherits.

**It is whole-store, so it takes no `projectId`.** `roadmap-data-model.md` § 6
is explicit: *"Acyclicity is checked over the full store only: a partial
checkout can break a cycle by not containing part of it, so checking there
would report a pass that the whole store fails."* A per-project signature is
that partial checkout in miniature — and it is reachable, because
`relateCrossProject()` stores `dst_project` + `dst_id_fold`, so `A(p1) → B(p2) →
A(p1)` is a cycle no per-project scan can see. `tests/features/roadmap_export_roundtrip/`
already builds a synthetic three-project fixture that calls
`relateCrossProject("blocked-by", …)` alongside an in-project `relateItems()`
edge of the same type, so the shape is one the corpus already produces and both
of § 2.2's edge sources are exercised by an existing fixture.

**It runs over four types, one type at a time.** The model names exactly four as
acyclic. The other two are excluded for different reasons and both need saying:
`relates-to` is the one **symmetric** type, stored once and normalised
(`RoadmapStore::relateItems()`), so a triangle of related items is an ordinary
undirected cycle and reporting it would be pure noise; `specified-by` addresses
a **document** by `dst_path`, so it contributes no item-to-item edge at all.
Types are never folded together — two items may legitimately be linked both
`blocked-by` and `duplicate-of`, and a folded graph reports that pair as a
cycle.

**A path element is a `(project, id)` pair, not a bare id.**
`roadmap-data-model.md` § 4.1 makes `id` unique only within its project, so a
whole-store walk that reported bare ids would produce an ambiguous path the
moment it crossed a project boundary.

```cpp
// src/roadmapcheck.h — declaring src/roadmapcheck.cpp, a new TU in
// ants_roadmapstore_lib. The first member of the health-check family
// ANTS-3794 owns; that id adds the rest and the scheduling. It is NOT
// placed beside ANTS-3793's reader seam: a graph check and a reader seam
// share nothing but a library, and the umbrella's own loop-2 tail recorded
// that co-location as cohesion invented after the fact.
namespace RoadmapCheck {

// One cycle, in path order, closing implicitly: `path` [A, B, C] means
// A → B → C → A.
struct RelationshipCycle {
    QString type;                             // one of the four acyclic types
    QVector<QPair<QString, QString>> path;    // (export_slug, id_fold), in order
};

struct AcyclicityReport {
    QVector<RelationshipCycle> cycles;  // empty ⇒ clean
    // Cross-project edges whose far project is absent from THIS store. Such an
    // edge cannot close a cycle here, and skipping it silently would let a
    // store missing a project report "clean" over a graph it could not see —
    // which is the failure § 6 of the model rules out by scoping to the full
    // store. Reported as a count so a caller can distinguish the two.
    int unresolvedEdges = 0;
};

// Reports; never refuses, never writes. An engaged report with an empty
// `cycles` is a CLEAN store; `nullopt` with `*error` set is a FAILED check,
// and the two must not be collapsed.
std::optional<AcyclicityReport> findRelationshipCycles(RoadmapStore &store,
                                                       QString *error = nullptr);

}  // namespace RoadmapCheck
```

**The walk.** Per type, one query over `relationship` joined to `item` and
`project` collects the edge list — same-project edges via `dst_pk`, cross-project
edges resolved from `(dst_project, dst_id_fold)` to an item in this store, and
`dst_path` edges skipped as document targets. Then an iterative three-colour DFS
over that list; a back edge yields the cycle, unwound from the stack in path
order. Iterative rather than recursive because the edge list is data-driven and
a recursive walk's depth is the store's, not the code's. It reads the database
through `RoadmapStore::db()`, which is in-library use of an in-library accessor
— the same thing `roadmapexport.cpp`'s `writeRelationships()` already does.

**It ships with no scheduled caller, deliberately.** The scheduling belongs to
ANTS-3794 along with the rest of the family. Until then it is reachable from its
own test and from a future check runner, and nothing calls it in production. A
declared, tested function with no caller is the correct intermediate state when
the id owning the cadence has not landed — stated here so its absence from any
run loop is not read as an oversight.

## 3. Invariants

**Renumbered from 1.** This document is a rewrite of the umbrella at a narrowed
scope, and carrying its sparse numbering into a four-invariant spec would read
as five missing invariants. The mapping, so the old citations stay findable:
**INV-1 was the umbrella's INV-7**, **INV-2 was its INV-8**. INV-3 and INV-4 are
new, and both come from grounding the two halves: INV-3 from § 2.1.2's vacuity
question, INV-4 from § 2.2's three corrections. ANTS-3793 and ANTS-3808 did the
same renumbering; `specs.md` § 5.5 keeps ids permanent *within* a document, and
a narrowed rewrite is a new contract.

- **INV-1** — **The full round trip loses nothing and invents nothing, over the
  facts markdown carries.** Render → `findRoadmaps()` → `planFrom()` →
  `RoadmapMigrateLoad::load()` → export, compared against the source store's
  export under § 2.1.1's projection applied to **both** sides. *Breaks when:* a
  field markdown does carry — `headline`, `status`, `kind`, `source`, `layman`,
  `body`, `lanes`, `evidence` — is dropped or altered by the render, an element
  is emitted out of order, or a section's `source_path` is not reproduced so the
  archive lands in the wrong file. *Test:* `Inv1RoundTrip`, verified red against
  the pre-ANTS-3808 `item.body` write (§ 2.1.2).
- **INV-2** — **A relationship cycle is reported, not refused and not ignored.**
  *Breaks when:* `relateItems()` starts rejecting a write that closes a cycle;
  the check reports only self-relationships, which the DDL `CHECK` already
  covers; or a failed check (`nullopt`) is collapsed with a clean one (an
  engaged report holding no cycles). *Test:* `Inv2Acyclicity`, which stores
  `A → B → A` under `blocked-by`, asserts **both** writes succeed, and asserts
  the returned report names the cycle in path order.
- **INV-3** — **The oracle discriminates.** A comparison of two projections that
  are both empty, or that omit the fields under test, passes against a render
  that does nothing. *Breaks when:* the fixture leaves a markdown-carried field
  unpopulated; the render's INV-5 gate fires, so both sides are near-empty and
  the failure is a diff rather than a diagnosis; or the projection predicate is
  widened until it excludes a field the render is supposed to carry. *Test:*
  `Inv3OracleDiscriminates`, which asserts `gateFailures.isEmpty()` and
  `committed` before comparing, asserts the projected set is non-empty and holds
  at least one `item`, `section` and `element` record, and asserts every field
  INV-1's *Breaks when* names is present in the projected `item` record.
- **INV-4** — **The check's domain is the whole store, one type at a time, over
  the four acyclic types.** *Breaks when:* the walk is scoped to one project, so
  a cycle closed by a `relateCrossProject()` edge is invisible; edges of
  different types are folded into one graph, so a pair linked `blocked-by` and
  `duplicate-of` is reported as a cycle; `relates-to` is included, so an ordinary
  triangle of related items is reported; or a cross-project edge whose far
  project is absent is skipped without being counted. *Test:* `Inv4CheckDomain`,
  four legs — a cross-project cycle over three projects (the fixture shape
  `roadmap_export_roundtrip` already builds), a doubly-typed pair asserted
  clean, a `relates-to` triangle asserted clean, and a dangling cross-project
  edge asserted clean with `unresolvedEdges == 1`.

## 4. RAM, latency and build cost

**The oracle is a test and never runs in production**, so it has no budget a
user pays. Its resident cost is two open `RoadmapStore`s and two exports; the
exports are written to `QBuffer`s over the fixture described in § 2.1.2 — a
handful of items, not the corpus. No figure is quoted for a corpus-scale round
trip because nothing runs one: the fixture is deliberately small, and quoting a
projected number for a path that does not exist would be a measurement with no
command behind it.

**The check's cost is bounded by the edges it walks, and today that set is
empty.** All four acyclic types are **authored-only** in
`roadmap-data-model.md` § 6's Migration column — migration harvests nothing for
them, `blocked-by` explicitly so (inferring a relationship from prose is what
that spec's INV-5 forbids). Only `relates-to` (~21 converted `Dependencies:`
values) and `specified-by` (~20 converted `Spec:` values) are populated by
migration, and § 2.2 excludes both. So on this project's store the check runs
over zero edges and returns clean; it is built ahead of its data, which is the
point of building it before someone starts authoring the edges by hand. The walk
is O(items + edges) in time and holds one `QVector` of edges plus the DFS stack.
For scale: this project's roadmap carries **1,659** bracket-id bullets, measured
2026-08-04 with `grep -cE '^- [^ ]+ \[[A-Z]+-[0-9]+\]' ROADMAP.md`, over a live
file and **2** rotated archives (`ls docs/roadmap/` → `0.5.md`, `0.6.md`).
An item-per-bullet store therefore walks a few thousand nodes at most — and the
bullet figure is an anchor rather than a constant, since the corpus grows on
every triage pass (it moved by one *while this spec was being drafted*).

**Build cost: one new TU, no new link edge.** `src/roadmapcheck.cpp` joins
`ants_roadmapstore_lib`, which already links `Qt6::Core Qt6::Sql` PUBLIC and
`ants_warnings` PRIVATE (`CMakeLists.txt`, the `ants_roadmapstore_lib` target).
The check needs nothing else — no parse, no render, no `projectsettings.cpp` —
so it adds a compile unit and nothing to the link surface. The oracle adds no
production TU at all. The feature test directory joins the `test_core` bundle's
`SOURCES`, which is a list entry rather than a target.

## 5. Out of scope

- **Scheduling, cadence and the rest of the health-check family** — **ANTS-3794**.
  This spec declares one check and ships it with no caller (§ 2.2).
- **Repairing anything the check finds.** It reports. Breaking a cycle is a
  human decision about which edge was wrong, and a check that guessed would
  destroy the record of the disagreement.
- **Whether the render should gain a `Resolution:` carrier** — **ANTS-3824**,
  filed 2026-08-04. § 2.1.1 excludes `resolution` from the projection, which is
  the right answer *for the oracle* and deliberately not an answer to the design
  question. The same applies to `priority`.
- **Format conformance.** The oracle proves losslessness and not that the render
  emits every required piece — ANTS-3758 § 2.6 gives the worked example (a
  render omitting `Kind:` on every `implement` item re-parses identically and
  passes this comparison), and its INV-12 owns that claim.
- **The export's own round trip** — export, rebuild, re-export, byte-identical.
  That is **ANTS-3761 INV-1**, a different contract with a different failure
  mode, already tested by `tests/features/roadmap_export_roundtrip/`. Named here
  because "the round-trip test" is ambiguous across the two.
- **The 102 missing `Layman:` lines on this project's own roadmap** —
  **ANTS-3821**. They would trip the render's INV-5 gate on a real publish; the
  oracle's fixture is its own and carries a `layman` on every public open item
  (§ 2.1.2), so this spec is not blocked by it.
- **Widening `ANTS-3797`'s column-diff discipline.** § 2.1.1's additions are to
  ANTS-3758 § 2.6's projection, not to the export's column check.

## 6. Tests

`tests/features/roadmap_round_trip/`, label `features`, compiled into the
**`test_core` bundle** per `tests/features/README.md` (no `add_executable`) —
the same bundle ANTS-3793 § 6, ANTS-3808 § 6 and ANTS-3809 § 6 use, and for the
same reason: it is the only bundle linking both `ants_core_lib` and
`ants_roadmapstore_lib` (`CMakeLists.txt`, the `test_core` bundle's `LIBS`).

| Case | Invariants |
|---|---|
| `Inv1RoundTrip` | INV-1 |
| `Inv2Acyclicity` | INV-2 |
| `Inv3OracleDiscriminates` | INV-3 |
| `Inv4CheckDomain` | INV-4 |

**The projection helper is shared by `Inv1RoundTrip` and
`Inv3OracleDiscriminates` and written once**, with its excluded families as an
enumerated list rather than a predicate lambda per call site — so a future
record kind or per-item field is a compile-or-fail rather than a silent
widening. ANTS-3758 § 2.6 asks for exactly this, and ANTS-3797 records the cost
of not having it: a column went uncarried in the export's own diff and the check
passed anyway.

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored** (`testing.md` owns the
mutation-harness rules, including mtime busting, and they are not restated
here). INV-1's mutation is named in § 2.1.2 and needs no new code: it is the
`item.body` write `src/roadmapmigrate.cpp` performs today. INV-2's, INV-3's and
INV-4's are run against the first implementation with the rule under test
removed — the report replaced by a refusal inside `relateItems()`, the gate and
non-empty assertions deleted, and the walk scoped to one project.

**The fixtures are this directory's own**, not reached out of
`tests/features/roadmap_migrate_archive_root/`, whose `spec.md` scopes it to
preamble round-tripping and lists bullet-body fidelity as out of scope — a case
here depending on its internals would couple two contracts that were
deliberately split. `Inv4CheckDomain`'s three-project shape is *modelled on*
`tests/features/roadmap_export_roundtrip/`'s fixture and built locally, for the
same reason.

Two rules worth restating because each is a silent trap rather than a
convention. **Never default-construct `RoadmapStore`**: it resolves
`defaultPath()`, the developer's real store under `XDG_DATA_HOME`, so every case
would write into it. Always
`std::make_unique<RoadmapStore>(dir.filePath("store.db"),
RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::…)` — `Access` is
the **third** parameter, after the history cap. And **the scratch store of
`Inv1RoundTrip` opens `Access::Bulk`**, because `RoadmapMigrateLoad::load()`
refuses an `Interactive` connection (§ 2.1, rule 1); the source store opens
`Access::Interactive` like any consumer's.

## 7. Cross-doc impact

- **ANTS-3758's INV-1 is REWORDED to the half its test proves**, and points the
  full oracle at this spec's INV-1. Rewording rather than annotating: annotating
  would leave two live statements of one contract, which is the conflict class
  the rule-14 gate exists to catch. `specs.md` § 5.5 permits it — the id is
  permanent, the wording is not.
- **ANTS-3758 INV-1's *Breaks when* clause loses `resolution` and `an extras
  key`.** § 2.1.1 shows both are unsatisfiable: the export emits them, markdown
  has no carrier, and the same spec's § 2.6 predicate must therefore exclude
  them. The clause keeps `layman`, `body`, `lanes` and `evidence`, all of which
  the render does carry.
- **ANTS-3758 § 2.6's family 3 gains `resolution`, `priority` and `extras`**,
  with the `extras` entry carrying § 2.1.1's reasoning — that the migration
  writes it from keys the render deliberately cannot reproduce, so it is the one
  member of the family that looks like it round-trips.
- **ANTS-3756's schema comment and § 5 both name the wrong owner.** The DDL
  comment reads *"the whole-store acyclicity check ANTS-3758 owns"*
  (`docs/specs/ANTS-3756-roadmap-store-schema.md`) and its § 5 files the check
  out to ANTS-3758; both become **ANTS-3810**. The DDL in `src/roadmapstore.cpp`
  carries the same `CHECK` with **no comment at all**, so it needs no change —
  the stale pointer lives only in the spec.
- **`tests/features/roadmap_render/test_roadmap_render.cpp`'s comment above
  `Inv1ExportsMatch` names the wrong id.** It reads *"what ANTS-3793's cutover
  work wires up"*, written before the four-way split moved the oracle here; it
  becomes **ANTS-3810** in the same change that rewords INV-1. An implementer
  following that comment today lands in the read seam.
- **`roadmap-data-model.md` § 6 gains a pointer to this spec** as the id
  implementing its acyclicity rule. The rule itself is unchanged — § 2.2
  conforms to it, including the whole-store scope that corrected the umbrella's
  per-project signature.
- **`src/roadmapcheck.h` / `.cpp` are new**, and `docs/subsystems.md` gains a
  `roadmapcheck` entry beside `roadmapexport` and `roadmaprender`. `CLAUDE.md`
  is unaffected — ANTS-1292 moved the per-file catalogue out of it.
- **`CMakeLists.txt`** gains `src/roadmapcheck.cpp` in `ants_roadmapstore_lib`
  and `tests/features/roadmap_round_trip/test_roadmap_round_trip.cpp` in the
  `test_core` bundle's `SOURCES`.
- **ANTS-3794 inherits the family this spec opens**, and its bullet's claim that
  *"INV-1 already fixes the export round-trip check"* refers to **ANTS-3761's**
  INV-1, not ANTS-3758's. The two are distinguished in § 5; the bullet is
  annotated on ship so a reader of that id does not conclude the render's round
  trip is covered.
- **ANTS-3824** (filed 2026-08-04) owns the design question § 5 excludes.
  Nothing in this spec blocks on it.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 0-split | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`, carrying its §§ 2.6–2.7 and INV-7 / INV-8.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. The umbrella carried seven contracts and stopped at `/cold-eyes`' cap with collateral outnumbering draft defects two loops running; the user split it four ways on 2026-08-03 (ANTS-3793 read seam, ANTS-3808 `item.body`, ANTS-3809 write half, ANTS-3810 this). Invariants renumbered from 1 with the mapping in § 3. Drafting also changed three inherited claims against source rather than carrying them: § 2.2's signature dropped its `projectId` (the model's § 6 scopes acyclicity to the full store), § 2.1.1 added three fields to ANTS-3758 § 2.6's projection, and § 2.1.2 replaced the umbrella's cross-id build-order constraint with the mutation harness. |
