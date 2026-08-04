# ANTS-3810 — the round-trip oracle, and whole-store relationship acyclicity

**Status:** draft (2026-08-04) — the rule-14 cold-eyes gate has not run.
**Kind:** test.
**Source:** ROADMAP.md ANTS-3810 (ANTS-3793 cold-eyes loop-3 split, 2026-08-03).
Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`,
which the user cut four ways after its cold-eyes run stopped at the loop cap.
This part carries the umbrella's §§ 2.6–2.7 and its INV-7 / INV-8, renumbered
from 1 (mapping in § 3). No reviewer was dispatched to produce the split; it is
a document operation, and the loop log's `0-split` row records only that.
**Covers:** ANTS-3810 only.
**Depends on, all shipped:** ANTS-3758 (the render this oracle drives),
ANTS-3765 (the migration loader), ANTS-3761 (the export). Nothing in § 2 needs
ANTS-3793, ANTS-3808 or ANTS-3809 to have landed, so this spec can be
**implemented** at any point.
**But `Inv1RoundTrip` does not go green until ANTS-3808 ships, and that is a
scheduling fact an implementer must have before starting.** § 2.1.2 removes the
umbrella's *build-before* constraint and this replaces it: the oracle's whole
purpose is to fail on the `item.body` defect, and that defect is in today's
code, so the case is **expected RED on arrival**. That is the discrimination
proof — got for free, no mutation needed — and it means the case is committed to
a green suite only once ANTS-3808's fix lands. Building the oracle first and
committing it green are two different moments; only the second is ordered.
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

**This spec builds ANTS-3758 § 2.6's contract and changes none of its
*substance*.** What it adds is what building the thing forced: three missing
exclusions and one carve-out in § 2.1.1, the comparison relation § 2.6 never
states (below), the four call-shape rules the pipeline imposes, and INV-3 /
INV-4, which are this spec's own. The pipeline, with every symbol resolved:

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
has to reproduce the layout rather than hand the loader one string.

**The column is `section.source_path`; the export record's key is `source`** —
and the difference is deliberate, not a typo to normalise. `roadmapexport.cpp`'s
section emitter says so in place: *"The key drops the `_path` suffix the way
`parent` drops `_id`; it does not collide with `item.source`, a different record
type."* A projection helper keyed on the wrong one of those two names fails
silently, so both are spelled here once: **write `source_path` when you mean the
column, `source` when you mean the emitted key.** That key is the field the
scratch layout proves.

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

**The comparison relation, which § 2.6 leaves to inference.** That section says
"byte-identical" of two whole exports and then introduces projection without
saying what projecting *does* to a line or what equality survives it. Both
halves are load-bearing and both are pinned here:

- **The projection operates at two levels, and which one applies is decided by
  the family.** Families 1 and 2 drop **whole NDJSON lines** — an excluded item
  takes its `item` line and its `element` line with it, and an excluded record
  kind is dropped wholesale. Family 3 drops **keys from a surviving `item`
  object**, which is then re-serialised through
  `RoadmapStore::canonicalJson()` — the same canonicaliser the export itself
  uses, so the projected line is byte-comparable rather than merely
  semantically equal.
- **The assertion is byte-equality of the projected line *sequences*, order
  included** — not multiset equality of the lines. This is the half that must
  not be got wrong: INV-1's *Breaks when* names *"an element is emitted out of
  order"* as a break, and a multiset comparison cannot see reordering at all.
  An implementer who reaches for set equality because it produces a friendlier
  diff silently deletes one of the two failures the invariant exists to catch.

The export's own record order is total and deterministic (ANTS-3761 § 2.4), so
sequence equality is a contract the export can actually meet — it is not a
stricter bar invented here.

#### 2.1.1 What the projection excludes, and three fields § 2.6 misses

ANTS-3758 § 2.6 enumerates three families and is cited rather than restated:
(1) items the render excludes by design — `visibility = 'internal'` and
`status = 'dropped'`; (2) record kinds markdown does not carry — `history`,
`relationship`, `citation`, `feedback_ref` and the `id_prefix` high-water;
(3) per-item fields the export emits and markdown has no carrier for —
`id_origin`, `provenance`, `created` / `last_modified` / `shipped`, and
`milestone`.

**`legend` stays inside the comparison, and saying so matters because it is the
one record kind family 2 could plausibly have swallowed.** `writeProject()`
emits a `legend` record; `RoadmapRender::renderLegend()` writes the legend into
the live file; and `MigrationPlan` carries a `std::optional<PlannedLegend>` to
read it back. So it round-trips through markdown like any item field, it is not
in family 2's list, and the § 2.1.2 fixture seeds one so the claim is exercised
rather than assumed.

**Family 2's `relationship` exclusion is sound today and rests on an
unimplemented conversion — so it gets a stated trigger rather than a blanket
line.** `roadmap-data-model.md` § 6's Migration column marks two of the six
types as markdown-carried: `relates-to` *"converted from `Dependencies:`
(~21 occurrences)"* and `specified-by` *"converted from `Spec:`
(~20 occurrences)"*. If that conversion existed, excluding every `relationship`
record would blind the oracle to the render dropping those trailers — exactly
the "facts markdown carries" class INV-1 protects. **It does not exist.**
Measured 2026-08-04: `grep -rn 'relateItems\|relateCrossProject' src/` returns
**no call site outside `roadmapstore.cpp` itself**, and `PlannedItem`
(`src/roadmapmigrate.h`) carries no relationship field — so the migration writes
zero relationships and neither side of the comparison holds one. The exclusion
is therefore correct as it stands, and correct *for a reason that can expire*:

> **Trigger.** When `Dependencies:` / `Spec:` conversion is implemented,
> `relates-to` and `specified-by` move **out** of family 2 and into the compared
> set, and INV-1's *Breaks when* gains them. The remaining four types are
> authored-only (§ 6's own column) and stay excluded permanently.

The standard describing a conversion the code does not perform is a defect in
its own right, and it is a code-side question this spec does not settle — filed
as **ANTS-3827**.

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
`Kind: fix`, never the `bugfix` it was mapped from (`mappedKind()` carries that
exact pair) — so the raw token is unrecoverable from the rendered text by
construction. `roadmapmigrate.cpp`'s own comment says as much about
`source_status`: *"storing the normalised word would lose `completed` vs `done`,
which the write-back being a right-inverse makes unrecoverable."* Export A
carries `{"source_kind":"bugfix"}`, export B carries `{}`, and they differ for a
reason that is the format's, not the render's. Family 3 is where it belongs.

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
`item`, one `section`, one `element` and one `legend` record. Concretely it
carries: an item with `body`, `layman`, `source`, `lanes` and `evidence` all
set; a second item in the same section whose insertion order differs from its
position; a nested section; a stored legend; and one archive section with a
`source_path`, so the scratch layout is exercised rather than assumed.

**The red proof costs nothing, because the defect is still in the tree.** The
umbrella required this oracle to be *built before* ANTS-3808's fix and shown red
against it, on the correct grounds that a fixture only ever run against
corrected code proves the oracle compiles rather than that it discriminates.
That grounds a *property*, not an ordering — and the property holds either way:

- **Before ANTS-3808 lands**, `src/roadmapmigrate.cpp` still writes the whole
  bullet into `item.body`, so `Inv1RoundTrip` is RED the first time it runs.
  No mutation is needed and none is contrived; the header states the
  consequence, which is that the case is committed to a green suite only after
  that id ships.
- **After ANTS-3808 lands**, the red proof is the ordinary § 6 mutation —
  restore the pre-fix `item.body` write — and it is the same fixture either way.

So there is no *build*-order constraint between the two ids, only a
*commit-green* one, and the discrimination proof survives whichever lands first.
(§ 6 owns the general convention; it is not restated here.)

### 2.2 Whole-store relationship acyclicity

**It is a check, not a constraint.** SQLite cannot express graph reachability in
DDL, and enforcing acyclicity inside `relateItems()` would put a traversal in
the write path of the migration's hottest loop. It runs over a finished store
and **reports** rather than refuses — a cycle is a data fault to surface, not a
write to reject after the fact.

**Three corrections to the shape the umbrella sketched**, each because the
sketch contradicts a document or a column it inherits — followed by one thing
the sketch simply left open, the cycle-enumeration policy.

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

**A path element is an `(export_slug, id_fold)` pair, not a bare id.**
`roadmap-data-model.md` § 4.1 makes `id` unique only within its project, so a
whole-store walk that reported bare ids would produce an ambiguous path the
moment it crossed a project boundary. **Those two names are the exact ones, and
they are not the obvious ones** — the project side is `project.export_slug`
(what `writeProject()` addresses a project by), and the item side is `id_fold`,
the **case-folded** form of the authored id, which is what `relationship` rows
and every cross-project reference already carry. So a reported path element for
`[ANTS-3810]` reads `("ants-terminal", "ants-3810")`, not the authored spelling.
`relateItems()` already normalises `relates-to` on exactly this pair, so the
check reports items the way the store identifies them.

**Cycles are enumerated one per back edge, per type, and the report is
bounded.** Three decisions an implementer would otherwise make silently, and
INV-2 / INV-4 assert on the output of all three:

- **Rotation is canonical**: each cycle's `path` starts at its
  lexicographically smallest `(export_slug, id_fold)` element. A DFS may reach
  `A → B → A` from either end, so without this the same cycle is `[A,B]` or
  `[B,A]` depending on visit order and no test can assert on it.
- **One cycle per back edge**, first found wins — *not* every elementary cycle,
  which is exponential in the edge count and would make a health check a
  liability. A store with a dense tangle reports one cycle per back edge, which
  is enough to name the fault.
- **`kMaxCyclesPerType = 64`.** At the cap the walk stops **for that type**,
  `cycles` holds 64 entries for it, and the check still returns engaged —
  `AcyclicityReport::truncated` is set true so a caller can say "at least 64"
  rather than "64". A silent cap would report a bounded count as a complete
  one, which is the same silent-fallback failure `unresolvedEdges` exists to
  prevent.

```cpp
// src/roadmapcheck.h — declaring src/roadmapcheck.cpp, a new TU in
// ants_roadmapstore_lib. The first member of the health-check family
// ANTS-3794 owns; that id adds the rest and the scheduling. It is NOT
// placed beside ANTS-3793's reader seam: a graph check and a reader seam
// share nothing but a library, and the umbrella's own loop-2 tail recorded
// that co-location as cohesion invented after the fact.
namespace RoadmapCheck {

// One cycle, in path order, closing implicitly: `path` [A, B, C] means
// A → B → C → A. Rotated so path[0] is the smallest element, so the same
// cycle has one representation whatever order the walk reached it in.
struct RelationshipCycle {
    QString type;                             // one of the four acyclic types
    QVector<QPair<QString, QString>> path;    // (export_slug, id_fold), in order
};

struct AcyclicityReport {
    QVector<RelationshipCycle> cycles;  // empty ⇒ clean
    // ANY cross-project edge that does not resolve to an item in THIS store —
    // far project absent, or far project present but dst_id_fold matching no
    // item. One counter for both, deliberately: the caller's response is the
    // same (this store cannot see the whole graph), and a split would invite
    // the reading that the uncounted shape is fine. Such an edge cannot close
    // a cycle here, and skipping it silently would let a store missing a
    // project report "clean" over a graph it could not see — the failure § 6
    // of the model rules out by scoping to the full store.
    int unresolvedEdges = 0;
    // True when any type hit kMaxCyclesPerType, so a caller reads `cycles` as
    // "at least this many" rather than "this many".
    bool truncated = false;
};

inline constexpr int kMaxCyclesPerType = 64;

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
over that list; a back edge yields the cycle, unwound from the stack and then
rotated to its canonical start. Iterative rather than recursive because the edge
list is data-driven and a recursive walk's depth is the store's, not the code's.
It reads the database through `RoadmapStore::db()` — in-library use of an
in-library accessor, which is how the export path already reaches the same
table: `writeRelationships()` takes a `QSqlDatabase &` its caller obtained the
same way.

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
question, INV-4 from § 2.2's corrections. ANTS-3793 and ANTS-3808 did the
same renumbering; `specs.md` § 5.5 keeps ids permanent *within* a document, and
a narrowed rewrite is a new contract.

- **INV-1** — **The full round trip loses nothing and invents nothing, over the
  facts markdown carries.** Render → `findRoadmaps()` → `planFrom()` →
  `RoadmapMigrateLoad::load()` → export, compared against the source store's
  export under § 2.1.1's projection applied to **both** sides. *Breaks when:* a
  field markdown does carry — `headline`, `status`, `kind`, `source`, `layman`,
  `body`, `lanes`, `evidence` — is dropped or altered by the render, an element
  is emitted out of order, or a section's `source_path` is not reproduced so the
  archive lands in the wrong file. The comparison is **byte-equality of the
  projected line sequences, order included** (§ 2.1). *Test:* `Inv1RoundTrip`,
  RED against the `item.body` write in the tree today (§ 2.1.2).
- **INV-2** — **A relationship cycle is reported, not refused and not ignored.**
  *Breaks when:* `relateItems()` starts rejecting a write that closes a cycle;
  the check reports only self-relationships, which the DDL `CHECK` already
  covers; or a failed check (`nullopt`) is collapsed with a clean one (an
  engaged report holding no cycles). *Test:* `Inv2Acyclicity`, which stores
  `A → B → A` under `blocked-by`, asserts **both** writes succeed, and asserts
  the report holds exactly one cycle whose `path` equals the two endpoints in
  canonical rotation — smallest `(export_slug, id_fold)` first (§ 2.2), which is
  what makes the assertion deterministic rather than DFS-order-dependent.
- **INV-3** — **The oracle discriminates.** A comparison of two projections that
  are both empty, or that omit the fields under test, **would** pass against a
  render that does nothing — so the case asserts against that directly. *Breaks
  when:* the fixture leaves a markdown-carried field unpopulated; the render's
  INV-5 gate fires, so both sides are near-empty and the failure is a diff
  rather than a diagnosis; or the projection predicate is widened until it
  excludes a field the render is supposed to carry. *Test:*
  `Inv3OracleDiscriminates`, which asserts `gateFailures.isEmpty()` and
  `committed` before comparing; asserts the projected set is non-empty and holds
  at least one `item`, `section`, `element` and `legend` record; asserts every
  **item** field INV-1's *Breaks when* names is present on **each** projected
  `item` record; and asserts the two non-item breaks separately — that the
  element sequence is order-sensitive, and that the archive section's `source`
  key is present and non-null.
- **INV-4** — **The check's domain is the whole store, one type at a time, over
  the four acyclic types.** *Breaks when:* the walk is scoped to one project, so
  a cycle closed by a `relateCrossProject()` edge is invisible; edges of
  different types are folded into one graph, so a pair linked `blocked-by` and
  `duplicate-of` is reported as a cycle; `relates-to` is included, so an ordinary
  triangle of related items is reported; or a cross-project edge that does not
  resolve to an item in this store is skipped without being counted. *Test:*
  `Inv4CheckDomain`, five legs — a cross-project cycle over three projects (the
  fixture shape `roadmap_export_roundtrip` already builds), a doubly-typed pair
  asserted clean, a `relates-to` triangle asserted clean, and **both**
  unresolved shapes asserted clean with `unresolvedEdges == 1` each: far project
  absent, and far project present with a `dst_id_fold` matching no item.

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
them, `blocked-by` explicitly so — inferring a relationship from prose is what
`roadmap-data-model.md`'s own **INV-5** (*"A mentioned ID is not a
relationship"*) forbids. Note that is a **different** INV-5 from the render's
publish gate cited in § 2.1.2; both are named with their owning document here
for that reason. The model marks the other two types as converted —
`relates-to` from ~21 `Dependencies:` values, `specified-by` from ~20 `Spec:`
values — and § 2.2 excludes both anyway; § 2.1.1 records that **no conversion is
implemented**, so the edge set is empty from both directions. The check
therefore runs over zero edges on this project's store and returns clean. It is
built ahead of its data, which is the point of building it before someone starts
authoring edges by hand.

The walk is O(items + edges) in time and holds one `QVector` of edges plus the
DFS stack, capped at `kMaxCyclesPerType` results per type. For scale, this
project's roadmap carries **a few thousand** bracket-id bullets — measured
2026-08-04 at just over 1,650 with
`grep -cE '^- [^ ]+ \[[A-Z]+-[0-9]+\]' ROADMAP.md`, which counts the **live
file only**; the **2** rotated archives (`ls docs/roadmap/` → `0.5.md`,
`0.6.md`) hold more and are not in that number. **The figure is deliberately
imprecise**: it moved three times while this spec was being drafted, once for
each item the drafting itself filed, so an exact count would be stale before the
gate finished and would read as authoritative anyway. The argument needs the
magnitude; it never needed the digits.

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
  question. **`priority` is out of scope for a different reason and is not
  ANTS-3824's**: `roadmap-format.md` already answers it — position *is* priority
  — so there is no carrier question to decide, only a column nothing writes.
- **Implementing `Dependencies:` / `Spec:` → relationship conversion** —
  **ANTS-3827**, filed 2026-08-04. § 2.1.1 states the trigger that would move
  `relates-to` and `specified-by` out of the projection's exclusion set when it
  ships; deciding whether it *should* ship is that id's.
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
`ants_roadmapstore_lib` **today** (`CMakeLists.txt`, the `test_core` bundle's
`LIBS`). The qualifier is ANTS-3793's and is kept: it is a fact about the
current bundle list, not a property of the bundle.

**The directory is `roadmap_round_trip`, one underscore away from the existing
`roadmap_export_roundtrip`, and the two are different contracts** — this one is
render → load → export (ANTS-3758's INV-1), that one is export → rebuild →
re-export (ANTS-3761's INV-1). The near-collision is called out because § 5
already has to disambiguate "the round-trip test" in prose, and a reader
skimming `tests/features/` sees only the directory names.

All four cases live in the GTest suite **`RoadmapRoundTrip`**, named for the
directory. The bundle is shared, so the suite name is what separates these from
`TEST(RoadmapRender, Inv1ExportsMatch)` — which is the very case § 1 says
over-claims, and which a `ctest -R` on a bare case name would otherwise sweep up
alongside them.

| Case | Invariants |
|---|---|
| `RoadmapRoundTrip.Inv1RoundTrip` | INV-1 |
| `RoadmapRoundTrip.Inv2Acyclicity` | INV-2 |
| `RoadmapRoundTrip.Inv3OracleDiscriminates` | INV-3 |
| `RoadmapRoundTrip.Inv4CheckDomain` | INV-4 |

**The projection helper is shared by `Inv1RoundTrip` and
`Inv3OracleDiscriminates` and written once**, with its excluded families as an
enumerated list rather than a predicate lambda per call site — so a future
record kind or per-item field is a compile-or-fail rather than a silent
widening. ANTS-3758 § 2.6 asks for exactly this, and ANTS-3797 records the cost
of not having it: a column went uncarried in the export's own diff and the check
passed anyway.

Per this project's convention, **every case is verified RED against its *Breaks
when* mutation before the implementation is restored** — `testing.md` § 2.2
(*"Verify the test fails on broken code"*) owns that rule and it is not restated
here. **It does not own an mtime-busting rule**, contrary to what three sibling
specs assert: `grep -rni mtime docs/standards/testing.md` returns nothing.
Restoring a mutated source by copying a file with an older timestamp lets ninja
skip the rebuild, so the mutation stays in a green-linking binary — a real trap,
but a harness practice with no standard behind it, so it is stated here as
practice rather than cited as a rule. The mis-citation in the siblings is filed
as **ANTS-3826**, which also carries the better remedy — give the rule a home in
`testing.md` § 2.2, so their citation becomes correct rather than deleted.
INV-1's mutation is named in § 2.1.2 and needs no new code: it is the
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

One rule worth restating because it is a silent data-loss trap rather than a
convention: **never default-construct `RoadmapStore`**. It resolves
`defaultPath()`, the developer's real store under `XDG_DATA_HOME`, so every case
would write into it. Always
`std::make_unique<RoadmapStore>(dir.filePath("store.db"),
RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::…)`.

Which `Access` each store takes is § 2.1's rule 1 and is not restated here: the
source store is `Interactive`, `Inv1RoundTrip`'s scratch store is `Bulk`.

## 7. Cross-doc impact

- **ANTS-3758's INV-1 gets ONE amendment, in the single form `specs.md` § 5.5
  prescribes** — the annotation `INV-1 amended by ANTS-3810`, naming this spec's
  INV-1 as where the full round-trip claim now lives. That one annotation covers
  **both** changes below; they are not two separate licences to edit, and the
  distinction matters because the two look like opposite instructions:
  - **The claim is narrowed** to the half `Inv1ExportsMatch` proves.
  - **The *Breaks when* clause loses `resolution` and `an extras key`.**

  **Why trimming the clause is not the "reflowing" § 5.5 forbids.** That rule
  protects two things — an `INV-N`'s **id → meaning** mapping, and the **list**
  of invariants. A *Breaks when* clause is neither: it is diagnostic prose
  enumerating inputs that falsify the claim, and an entry naming an input that
  **cannot** falsify it is simply wrong. § 2.1.1 shows both are exactly that —
  the export emits them, markdown has no carrier, so the same spec's § 2.6
  predicate must exclude them, and INV-1 as written is unsatisfiable for any
  item carrying either. Removing an impossible break narrows nothing an
  implementer could have relied on. The annotation is what records that this
  happened; the umbrella proposed a bare reword instead, and the standard's own
  form answers its objection — an annotation that says *where the wider claim
  went* leaves one live statement and a pointer, not two claims. Nothing here
  touches ANTS-3758's list of invariants.

  The clause keeps `layman`, `body`, `lanes` and `evidence`, all of which the
  render does carry. **ANTS-3765's INV-3 corroborates from the loader's side**:
  it names `milestone`, `resolution`, `visibility` and `priority` as fields a
  re-run must never clear *because the plan cannot carry them*. **Three of those
  four**, not all: `visibility` is handled by family 1 rather than by exclusion
  (§ 2.1.1), since the column admits only `public` and `internal`
  (`CHECK (visibility IN ('public','internal'))`) and family 1 has already
  removed every `internal` item before the comparison runs.
- **ANTS-3758 § 2.6's family 3 gains `resolution`, `priority` and `extras`**,
  with the `extras` entry carrying § 2.1.1's reasoning — that the migration
  writes it from keys the render deliberately cannot reproduce, so it is the one
  member of the family that looks like it round-trips.
- **ANTS-3758 § 2.6's family 2 gains the relationship carve-out and its
  trigger.** As written it excludes every `relationship` record on the ground
  that markdown does not carry them, which is true of the four authored-only
  types and **not** of `relates-to` and `specified-by`, which
  `roadmap-data-model.md` § 6 marks converted. The exclusion is sound today only
  because that conversion is unimplemented (§ 2.1.1, **ANTS-3827**), and a
  blanket line records the conclusion while losing the reason — the precise
  shape of silent widening that section's own last paragraph warns against.
- **`roadmap-data-model.md` § 6's Migration column is unchanged by this spec and
  is the subject of ANTS-3827.** Recorded so a reader of § 2.1.1 does not take
  this document as having settled it: this spec asserts only that no conversion
  exists *today*, which is a measurement, not a decision about what should.
- **ANTS-3756's schema comment and § 5 both name the wrong owner.** The DDL
  comment reads *"the whole-store acyclicity check ANTS-3758 owns"*
  (`docs/specs/ANTS-3756-roadmap-store-schema.md`) and its § 5 files the check
  out to ANTS-3758; both become **ANTS-3810**. The DDL in `src/roadmapstore.cpp`
  carries the same `CHECK` with **no comment at all**, so it needs no change —
  the stale pointer lives only in the spec.
- **`tests/features/roadmap_render/test_roadmap_render.cpp`'s comment above
  `Inv1ExportsMatch` names the wrong id.** It reads *"what ANTS-3793's cutover
  work wires up"*, written before the four-way split moved the oracle here; it
  becomes **ANTS-3810** in the same change that amends INV-1. An implementer
  following that comment today lands in the read seam.
- **`roadmap-data-model.md` § 6 gains a pointer to this spec** as the id
  implementing its acyclicity rule. The rule itself is unchanged — § 2.2
  conforms to it, including the whole-store scope that corrected the umbrella's
  per-project signature.
- **`src/roadmapcheck.h` / `.cpp` are new**, and `docs/subsystems.md` gains a
  `roadmapcheck` entry. **It joins a catalogue that is already behind**: that
  file's roadmap entries are `roadmapdialog`, `roadmapparse`, `roadmapmigrate`
  and `roadmapmigrateload` (`grep -n '^- \`roadmap' docs/subsystems.md`, four
  hits, 2026-08-04) — `roadmapstore`, `roadmapexport` and `roadmaprender` have
  no entry at all despite having shipped. That gap is pre-existing and is filed
  as **ANTS-3825**, not fixed here; this spec adds its own entry only.
  `CLAUDE.md` is unaffected — ANTS-1292 moved the per-file catalogue out of it.
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
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, ~24k of bounded windows) | 1 / 3 / 8 / 15 / 0 | **27 verified, 27 fixed, 0 dismissed, 2 re-graded.** Dimension tally: dim 6×6, dim 4×5, dim 5×4, dim 7×4, dim 1×2, dim 2×2, dim 10×2, dim 11×1, dim 13×1. All 27 are draft defects — loop 1 has no prior fixes to generate collateral. **Both lanes led on a CRITICAL and they were different ones**, which is the two-lane roll earning its cost. Lane A's survived: the header said no id blocks § 2, while § 2.1.2 named *today's code* as INV-1's red mutation — so `Inv1RoundTrip` is genuinely RED until ANTS-3808 ships and an implementer would have committed a failing case into a suite this project keeps fully green. The spec had removed the umbrella's *build-before* ordering and silently created a *commit-green-after* one; both are now stated. Lane B's was **re-graded to HIGH on verification**: it argued family 2's blanket `relationship` exclusion blinds the oracle to ~41 markdown-carried edges, correct in principle — `roadmap-data-model.md` § 6 does mark `relates-to` and `specified-by` converted — but `grep -rn 'relateItems\|relateCrossProject' src/` returns **no call site outside `roadmapstore.cpp`**, and `PlannedItem` carries no relationship field, so nothing converts them and neither side holds one. The exclusion is sound *today* and now ships with a stated trigger instead of a blanket line; the standard-vs-code divergence is filed as ANTS-3827. The other HIGHs were both real contract gaps: the **comparison relation was never defined** (byte-vs-multiset, line-vs-key — and only the byte/sequence reading catches the element-ordering break INV-1 names), and § 7 told an implementer both to annotate and to edit ANTS-3758's INV-1. Also fixed: cycle rotation, multiplicity and bound were all unpinned while INV-2/INV-4 asserted on exact path output; `unresolvedEdges` had a second dangling shape in neither bucket. Two verified-WRONG author claims died here — the `source` vs `source_path` "inconsistency" is the export's own deliberate key naming (its emitter says so in a comment), and `bug → fix` is not in `mappedKind()`; `bugfix → fix` is. Doc grew 543 → 716 lines. Lane spend 107k / 107k cumulative across turns, ~58k on the first turn against the 60k per-turn budget. |
| 0-split | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`, carrying its §§ 2.6–2.7 and INV-7 / INV-8.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. The umbrella carried seven contracts and stopped at `/cold-eyes`' cap with collateral outnumbering draft defects two loops running; the user split it four ways on 2026-08-03 (ANTS-3793 read seam, ANTS-3808 `item.body`, ANTS-3809 write half, ANTS-3810 this). Invariants renumbered from 1 with the mapping in § 3. Drafting also changed three inherited claims against source rather than carrying them: § 2.2's signature dropped its `projectId` (the model's § 6 scopes acyclicity to the full store), § 2.1.1 added three fields to ANTS-3758 § 2.6's projection, and § 2.1.2 replaced the umbrella's cross-id build-order constraint with the mutation harness. |
