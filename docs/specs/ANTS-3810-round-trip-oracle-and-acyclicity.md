# ANTS-3810 — the round-trip oracle, and whole-store relationship acyclicity

**Status:** accepted (2026-08-04) — rule-14 gate run to its 3-loop cap, no
deferred tail. Two caveats a reader should have: **loop 3's own fixes were not
themselves cold-read**, which is what the cap means; and collateral outran draft
defects for two loops, so a further split is available and is the user's call.
The loop log carries the numbers.
**Kind:** test.
**Source:** ROADMAP.md ANTS-3810 (ANTS-3793 cold-eyes loop-3 split, 2026-08-03).
Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`,
which the user cut four ways after its cold-eyes run stopped at the loop cap.
This part carries the umbrella's §§ 2.6–2.7 and its INV-7 / INV-8, renumbered
from 1 (mapping in § 3). No reviewer was dispatched to produce the split; it is
a document operation, and the loop log's `0-split` row records only that.
**Covers:** ANTS-3810 only.
**Depends on, all shipped:** ANTS-3758 (the render this oracle drives),
ANTS-3765 (the migration loader), ANTS-3761 (the export). Implementable at any
point — nothing in § 2 needs ANTS-3793, ANTS-3808 or ANTS-3809 to have landed.
**Commit-order dependency:** ANTS-3808 — build the oracle any time, but
`Inv1RoundTrip` is expected RED until that fix ships, so it joins a green suite
only afterwards. § 2.1.2 owns why. This is not a *shipped* dependency like the
three above, which is why it is named separately rather than folded into them.
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
states (below), the five call-shape rules the pipeline imposes, and INV-3 /
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

**Five call-shape rules the pipeline imposes, each from the code and none
optional.**

1. **The scratch store opens on `Access::Bulk`; the source store opens
   `Access::Interactive` like any consumer's.** `RoadmapMigrateLoad::load()`
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
4. **`Options::liveRoadmapPath` points at the live file *under the scratch
   root*, and `dryRun` is false.** `roadmaprender.h` marks the field REQUIRED
   and says why — the render's library does not link `projectsettings.cpp`, so
   it cannot resolve the `roadmap` override itself. Point it outside the scratch
   root and the render writes the live roadmap somewhere `findRoadmaps()` will
   never look, which empties side B for a reason that looks like data loss.
5. **The scratch root does not leak into the comparison**, and this is checked
   across **both** paths a root could escape by, not just the obvious one.
   - The `meta` record emits `schema`, `project` and `name` and **no root**
     (`src/roadmapexport.cpp`, the meta emitter); `registerProject()`
     canonicalises the root into a column the export never reads.
   - `section.source_path` is the other path, and it is the one that looks
     dangerous: the export emits it (as the key `source`), and INV-1 names it
     as a break. It is safe because the loader stores it **project-relative** —
     `relativeSourcePath()` (`src/roadmapmigrateload.cpp`) computes
     `QDir(canonical projectRoot).relativeFilePath(canonical sourcePath)`, with
     both sides canonicalised, per ANTS-3782 § 2.4 / INV-28. So an archive
     stored as `docs/roadmap/0.6.md` is the same string whichever root it was
     rendered under.

   Both are verified rather than assumed, because "the root does not leak" is
   the kind of claim that is true of the record everyone checks and false of
   the one nobody does.

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
  kind is dropped wholesale. **A `section` line is never dropped**, even when
  family 1 removed every item filed under it: the render emits a section
  regardless of whether any of its items survived `isRenderable()`
  (`src/roadmaprender.cpp` orders and routes sections independently of the item
  filter), so the heading reaches the markdown, the re-load parses it back, and
  the record is present on **both** sides. Dropping it on side A alone would
  manufacture the difference. Family 3 drops **keys from a surviving `item`
  object**, which is then re-serialised through **`JsonCanonical::serialise()`**
  — named precisely because it is the function `emitLine()` itself calls
  (`src/roadmapexport.cpp`), so a projected line and an untouched one went
  through the same RFC 8785 canonicalisation and are byte-comparable rather
  than merely semantically equal. (`RoadmapStore::canonicalJson()` produces the
  same canonical form for the store's JSON *columns*; it is not the function to
  reach for here, and naming the wrong one of the two yields a projection whose
  output only *usually* matches.)
- **The assertion is byte-equality of the projected line *sequences*, order
  included** — not multiset equality of the lines. This is the half that must
  not be got wrong: INV-1's *Breaks when* names *"an element is emitted out of
  order"* as a break, and a multiset comparison cannot see reordering at all.
  An implementer who reaches for set equality because it produces a friendlier
  diff silently deletes one of the two failures the invariant exists to catch.

The export's own record order is total and deterministic (ANTS-3761 § 2.4), so
sequence equality is a contract the export can actually meet — it is not a
stricter bar invented here.

**Family 3 is a per-`item` exclusion list, and nothing else is excluded at key
level.** Every field of every `section`, `element` and `legend` record stays
inside the comparison. This is worth stating because it tells an implementer
staring at a `section`-level difference which way to fix it: there is no
exclusion to widen, so the difference is a render defect.

#### 2.1.1 What the projection excludes, and three fields § 2.6 misses

ANTS-3758 § 2.6 enumerates three families and is cited rather than restated:
(1) items the render excludes by design — `visibility = 'internal'` and
`status = 'dropped'`, both of which the `item` DDL's CHECKs admit; (2) record
kinds markdown does not carry — `history`, `rel`, `citation`, `feedback_ref`
and the `id_prefix` high-water; (3) per-item fields the export emits and
markdown has no carrier for — `id_origin`, `provenance`, `created` /
`last_modified` / `shipped`, and `milestone`.

**The record kind is `rel`, not `relationship`** — `writeRelationships()` emits
`{"t":"rel",…}` and `relationship` is only the table name. The same
table-versus-emitted-key divergence as `source_path`/`source` above, and it
matters for the same reason twice over: § 6 requires the exclusions be an
**enumerated list** precisely so that widening is loud, and a list entry keyed
`"relationship"` matches nothing, silently. ANTS-3758 § 2.6 carries the wrong
name too; § 7 corrects it there.

**`legend` stays inside the comparison, and saying so matters because it is the
one record kind family 2 could plausibly have swallowed.** All three legs of the
round trip carry it: `writeProject()` emits a `legend` record; `render()` writes
the legend into the live file (via a file-local `renderLegend()` helper — it is
in an anonymous namespace inside `roadmaprender.cpp`, not a public
`RoadmapRender::` symbol); and the loader writes it back —
`if (plan.legend && !store.setLegend(projectId, plan.legend->entries, &err))`
in `src/roadmapmigrateload.cpp`. Naming the loader call is the load-bearing
part: the plan merely *carrying* a legend would not put one on side B, and
INV-3 asserts a `legend` record on both sides. The render's own comment states
the round trip as an intended property — *"Key order is the export's
kStatusOrder, so two renders of one store agree (INV-7) and a re-load reproduces
the same object (INV-1)"*.

**With one exception, and the fixture must respect it: a `dropped` legend entry
has no markdown form.** `renderLegend()` walks a fixed `kStatusOrder` of
`planned`, `in-progress`, `shipped`, `considered` and skips `dropped` outright,
because `roadmap-format.md` § 3.11 makes a fifth status emoji an anti-pattern.
So a store whose legend carries a `dropped` wording renders four entries,
re-loads four, and fails INV-1 on the fifth — a family-3-shaped loss at the
legend level rather than the item level. The fixture's legend therefore covers
**the four rendered statuses only**, and this is called out rather than left to
be discovered, because it is the one field in the compared set whose loss is by
design.

**Family 2's `rel` exclusion is sound today and rests on an unimplemented
conversion — so it gets a stated trigger rather than a blanket line.**
`roadmap-data-model.md` § 6's Migration column marks two of the six types as
markdown-carried: `relates-to` *"converted from `Dependencies:`
(~21 occurrences)"* and `specified-by` *"converted from `Spec:`
(~20 occurrences)"*. If that conversion existed, excluding every `rel` record
would blind the oracle to the render dropping those trailers — exactly the
"facts markdown carries" class INV-1 protects. **It does not exist.**
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
empty projections are equal. Three rules close that off — **assert every stage,
populate the fixture fully, and prove the oracle discriminates** — and each
answers the question *which rule makes this fixture fail, and is it the rule
under test?*

**Every stage of the pipeline is asserted before the comparison runs, not left
to fail the diff.** The render's INV-5 gate is the one that motivates the rule:
`render()` returns an *engaged* `Outcome` with `gateFailures` listing public
open items that carry no `layman`, and writes nothing (`src/roadmaprender.h`).
A fixture that trips it renders no files, so side B is near-empty — the
comparison fails, but it fails as an unreadable diff rather than as one line.

**The same failure shape belongs to every other stage, so every stage is
asserted — including the two that must be checked before the gate can be read
at all.** `render()` returns `std::optional<Outcome>`, and `nullopt` is reserved
for failures *before* the commit phase (SQL error, render error, path refusal);
reading `gateFailures` presupposes the optional is engaged, so an implementer
working from a list that starts at `gateFailures` dereferences a disengaged
optional on the one path this paragraph exists to make legible. `findRoadmaps()`
returns `nullopt` with a refusal code (`not_found` | `case_ambiguous` |
`not_utf8` | `archive_format_mismatch`); `load()` returns
`Outcome::ok == false` with nothing committed; and `writeProject()` returns
`false` on either side. Each empties or aborts a side exactly the way a tripped
gate does. So the oracle asserts, **in pipeline order and before it compares
anything**:

1. `render()` returned **engaged**
2. `RoadmapRender::Outcome::gateFailures.isEmpty()`
3. `RoadmapRender::Outcome::committed`
4. `findRoadmaps()` returned **engaged**
5. `RoadmapMigrateLoad::Outcome::ok`
6. both `RoadmapExport::writeProject()` calls returned **true**

Two distinct types are named `Outcome` here — `RoadmapRender::Outcome` and
`RoadmapMigrateLoad::Outcome` — so both are qualified wherever they appear.
Every public open item in the fixture carries a `layman`.

**The fixture populates every field family that survives the projection**, and
the case asserts the projected record set is non-empty and contains at least one
`item`, one `section`, one `element` and one `legend` record.

**Both items carry every markdown-carried field, and that is a requirement
rather than incidental generosity.** `layman` and `body` are emitted through
`insertIfPresent`, so an item holding neither emits **no such key** — and INV-3
asserts those fields on *each* projected `item` record. A fixture whose second
item omitted `body` would therefore fail against a perfectly correct render, and
the tempting repair (weaken the assertion to "at least one record") is precisely
the projection-widening INV-3's own *Breaks when* forbids. Fix the fixture, not
the assertion.

**And it populates the EXCLUDED families too — otherwise the projection's
exclusion arms are never executed, which is the same vacuity one level down.**
Populating only what survives tests only the *carrying* half: a helper that
forgot family 1 entirely, or that never stripped § 2.1.1's three family-3
additions, passes every case here. That is exactly the ANTS-3797 shape § 6
cites — a check that passes because it never reached the thing it was checking —
and it is what would make § 2.1.1's own `visibility` argument
("it round-trips *given* family 1, and only given it") untested.

Concretely the fixture carries:

| Purpose | Content |
|---|---|
| carried fields, per-record | two items in the same section, **each** with `body`, `layman`, `source`, `lanes` and `evidence` set |
| element ordering | the second item inserted in an order that differs from its position |
| section shape | a nested section, and one archive section with a `source_path` |
| legend | a stored legend over the four rendered statuses (§ 2.1.1) |
| **family 1** | one `visibility = 'internal'` item and one `status = 'dropped'` item, both filed in the section above |
| **family 3** | one item carrying `resolution`, `priority`, `milestone` and a migration-shaped `extras` key (`source_kind`) |

INV-3 asserts each family-1 item is **absent** from the projected set and each
family-3 key is **absent** from its item's projected record — the exclusion arms
executed, not merely declared.

**Three layout preconditions the fixture must meet, because `findRoadmaps()`
rediscovers from disk and each of these silently empties side B.** They belong
here rather than in § 2.1's call-shape rules because they constrain the
*fixture's data*, not the call:

1. **The live file is directly in the scratch root and its name case-folds to
   `roadmap.md`.** `findRoadmaps()` scans the root non-recursively for exactly
   that, so `docs/ROADMAP.md` is not a candidate and any other name is
   `not_found`. Two files differing only in case are `case_ambiguous`.
2. **Every archive `source_path` sits under `docs/roadmap/`**, which is the only
   directory archives are discovered in.
3. **Each section's stored `slug` equals what the migration derives from its
   title** — `RoadmapIndex::slugifyHeading()` then `uniqueSlug()`, with archive
   sections additionally namespaced by their file's prefix
   (`src/roadmapmigrate.cpp`). Markdown carries the heading *title*, never the
   slug, so a fixture that stores an arbitrary slug fails against a correct
   render for a reason that looks like a render defect.

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
Types are never folded together — two items may legitimately be linked
`A blocked-by B` **and** `B duplicate-of A`, and a folded graph reports that
pair as a cycle. **The opposing directions are the whole point**: same-direction
edges (`A blocked-by B`, `A duplicate-of B`) close no cycle even folded, so a
fixture built that way passes against a folded implementation and proves
nothing. INV-4's leg pins the directions for that reason.

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
bounded.** Three decisions an implementer would otherwise make silently. Each
gets its own assertion — rotation in INV-2, multiplicity and the cap in INV-4 —
because a decision pinned in prose and asserted nowhere is a decision the first
refactor un-pins:

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
    // True when ANY type hit kMaxCyclesPerType. One flag for four types, so it
    // says "at least kMaxCyclesPerType for some type" and NOT "cycles.size()
    // is a floor of the true total" — with two types capped, `cycles` already
    // holds 2 × kMaxCyclesPerType. A caller wanting the per-type breakdown
    // re-runs against a store it has narrowed; this report does not carry it.
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
  engaged report holding no cycles). *Test:* `Inv2Acyclicity`, three legs.
  (a) stores `A → B → A` under `blocked-by`, asserts **both** writes succeed,
  and asserts the report holds exactly one cycle whose `path` equals the two
  endpoints in canonical rotation — smallest `(export_slug, id_fold)` first
  (§ 2.2), which is what makes the assertion deterministic rather than
  DFS-order-dependent. (b) asserts the same store reports **no** cycle before
  the closing edge is written, so the report is a function of the graph and not
  of having been called. (c) drives the check against an **unopened** store and
  asserts `nullopt` with `*error` set — the leg that separates a failed check
  from a clean one, which the *Breaks when* names and nothing else exercises.
- **INV-3** — **The oracle discriminates.** A comparison of two projections that
  are both empty, or that omit the fields under test, **would** pass against a
  render that does nothing — so the case asserts against that directly. *Breaks
  when:* the fixture leaves a markdown-carried field unpopulated, or populates
  no member of an *excluded* family, so the projection's exclusion arms never
  run; the render's INV-5 gate fires, so **side B** is near-empty and the
  failure is a diff rather than a diagnosis; or the projection predicate is
  widened until it excludes a field the render is supposed to carry. *Test:*
  `Inv3OracleDiscriminates`, which asserts all six stage outcomes before
  comparing (§ 2.1.2's numbered list); asserts the projected set is non-empty
  and holds at least one `item`, `section`, `element` and `legend` record;
  asserts every **item** field INV-1's *Breaks when* names is present on
  **each** projected `item` record, which § 2.1.2 makes satisfiable by requiring
  both fixture items to carry all of them; asserts the **exclusion arms ran** —
  the `internal` and `dropped` items absent from the projected set, and
  `resolution` / `priority` / `milestone` / `extras` absent from the record of
  the item that carries them; and asserts the two non-item breaks separately —
  that the element sequence is order-sensitive, and that the archive section's
  `source` key is present and non-null.
- **INV-4** — **The check's domain is the whole store, one type at a time, over
  the four acyclic types.** *Breaks when:* the walk is scoped to one project, so
  a cycle closed by a `relateCrossProject()` edge is invisible; edges of
  different types are folded into one graph, so a pair linked `blocked-by` and
  `duplicate-of` is reported as a cycle; `relates-to` is included, so an ordinary
  triangle of related items is reported; or a cross-project edge that does not
  resolve to an item in this store is skipped without being counted; or the cap
  truncates without saying so. *Test:* `Inv4CheckDomain`, seven legs — a
  cross-project cycle over three projects (the fixture shape
  `roadmap_export_roundtrip` already builds); a doubly-typed pair
  (`A blocked-by B` **and** `B duplicate-of A`, the opposing directions § 2.2
  requires) asserted clean; a `relates-to` triangle asserted clean; **both**
  unresolved shapes asserted clean with `unresolvedEdges == 1` each — far
  project absent, and far project present with a `dst_id_fold` matching no
  item; a two-back-edge graph asserted to report **two** cycles, which is the
  one-per-back-edge rule (a one-per-SCC implementation reports one and passes
  every other leg); and a graph carrying more than `kMaxCyclesPerType` back
  edges **of a single type and no edges of any other**, asserted to yield
  `cycles.size() == kMaxCyclesPerType` and `truncated == true`. The single-type
  restriction is what makes the size assertion meaningful: `cycles` is one flat
  vector across all four types (§ 2.2), so a mixed fixture would have to filter
  by `type` to say anything.

## 4. RAM, latency and build cost

**The oracle is a test and never runs in production**, so it has no budget a
user pays. Its resident cost is two open `RoadmapStore`s and two exports; the
exports are written to `QBuffer`s over the fixture described in § 2.1.2 — a
handful of items, not the corpus. No figure is quoted for a corpus-scale round
trip because nothing runs one: the fixture is deliberately small, and quoting a
projected number for a path that does not exist would be a measurement with no
command behind it.

**INV-4's cap leg is the one case that is not small**, and it is called out so
"a handful of items" is not read as covering the whole suite: proving
`truncated` needs more than `kMaxCyclesPerType` back edges of one type, so on
the current value of 64 it builds on the order of 130 items. It is still a
generated in-memory store with no render and no export in the loop — cheap in
wall time, just not a handful.

**The check's cost is bounded by the edges it walks, and today that set is
empty.** All four acyclic types are **authored-only** in
`roadmap-data-model.md` § 6's Migration column, and the other two are excluded
by § 2.2 regardless — which § 2.1.1 shows is moot anyway, since no conversion is
implemented. So the edge set is empty from both directions: the check runs over
zero edges on this project's store and returns clean. It is built ahead of its
data, which is the point of building it before someone starts authoring edges by
hand. (`blocked-by` being authored-only is itself a rule —
`roadmap-data-model.md`'s **INV-5**, *"A mentioned ID is not a relationship"*.
That is a **different** INV-5 from the render publish gate § 2.1.2 cites; both
are named with their owning document wherever they appear here.)

The walk is O(items + edges) in time and holds one `QVector` of edges plus the
DFS stack. Its output is bounded at `kMaxCyclesPerType` cycles per type — and
each cycle's `path` is bounded by the number of distinct items in that type's
graph, since a simple cycle visits none twice, so the report is
O(items) worst case rather than genuinely unbounded. For scale, this project's
roadmap carries **roughly 1,650** bracket-id bullets in the live file, measured
2026-08-04 with `grep -cE '^- [^ ]+ \[[A-Z]+-[0-9]+\]' ROADMAP.md`; the **2**
rotated archives (`ls docs/roadmap/` → `0.5.md`, `0.6.md`) hold more and are
**not** counted by that command. **The figure is deliberately approximate**: it
moved three times while this spec was being drafted, once for each item the
drafting itself filed, so an exact count would be stale before the gate finished
and would read as authoritative anyway. The argument needs the magnitude; it
never needed the digits.

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

`tests/features/roadmap_round_trip/` — carrying a `spec.md` beside the test, per
the per-feature convention `CLAUDE.md` states. Label `features`, compiled into the
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
here. **It does not own an mtime-busting rule**, contrary to what two sibling
specs assert — ANTS-3793 § 6 and ANTS-3808 § 6, one occurrence each
(`grep -c 'mtime busting'` over the three siblings returns 1 / 1 / 0;
ANTS-3809 § 6 does not carry it). `grep -rni mtime docs/standards/testing.md`
returns nothing.
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

Which `Access` each store opens on is § 2.1's rule 1.

## 7. Cross-doc impact

- **ANTS-3758's INV-1 gets ONE amendment, in the single form `specs.md` § 5.5
  prescribes** — the annotation `INV-1 amended by ANTS-3810`, naming this spec's
  INV-1 as where the full round-trip claim now lives. That one annotation covers
  **both** changes below; they are not two separate licences to edit, and the
  distinction matters because the two look like opposite instructions:
  - **The claim is narrowed** to the half `Inv1ExportsMatch` proves.
  - **The *Breaks when* clause loses `resolution` and `an extras key`.**

  **Why one annotation is the whole licence needed.** § 5.5 says an `INV-N`'s
  meaning is amended *by annotating*, never by silent rewriting — so the
  narrowing is not an exception to the rule, it is the rule's own mechanism, and
  the annotation is what performs it. The clause trim needs no separate licence
  for a different reason: a *Breaks when* clause enumerates inputs that falsify
  the claim, and an entry naming an input that **cannot** falsify it is not a
  weaker claim but a wrong one. § 2.1.1 shows both are exactly that — the export
  emits them, markdown has no carrier, so the same spec's § 2.6 predicate must
  exclude them, and INV-1 as written is unsatisfiable for any item carrying
  either. The umbrella proposed a bare reword instead, and § 5.5's form answers
  its objection: an annotation that says *where the wider claim went* leaves one
  live statement and a pointer, not two claims. **Nothing here reflows
  ANTS-3758's list of invariants**, which is the other thing § 5.5 protects and
  the one this amendment genuinely does not touch.

  The clause keeps `layman`, `body`, `lanes` and `evidence`, all of which the
  render does carry. **ANTS-3765's INV-3 corroborates three of the fields
  § 2.1.1 EXCLUDES** — not the four just kept. It names `milestone`,
  `resolution`, `visibility` and `priority` as fields a re-run must never clear
  *because the plan cannot carry them*; three of those are family 3, and
  `visibility` is family 1's rather than family 3's for the reason § 2.1.1
  gives.
- **ANTS-3758 INV-1's *Breaks when* also keeps *"an element is emitted out of
  order"*, and the narrowing has to say why.** `Inv1ExportsMatch` is five
  positive `text.contains()` assertions over field values plus one negative;
  it cannot falsify element ordering any more than
  it can falsify a dropped `resolution`. By this section's own argument that
  would make it another impossible break to remove — but it is not, because the
  break is real and it is **this** spec's INV-1 that now carries it (§ 2.1's
  sequence-equality rule is exactly what makes it falsifiable). So the entry
  stays in ANTS-3758's clause as a statement about the *contract*, and the
  annotation's pointer is what tells a reader which document's test can fail on
  it. The two removed entries have no such home in either document; that is the
  distinction.
- **ANTS-3758 § 2.6's family 3 gains `resolution`, `priority` and `extras`**,
  with the `extras` entry carrying § 2.1.1's reasoning — that the migration
  writes it from keys the render deliberately cannot reproduce, so it is the one
  member of the family that looks like it round-trips.
- **ANTS-3758 § 2.6's family 2 gains two corrections**, both § 2.1.1's and
  neither restated here beyond its name: the record kind is **`rel`**, not
  `relationship`; and the exclusion gains the carve-out and trigger, because as
  written it excludes every relationship record on a ground that holds for the
  four authored-only types and not for `relates-to` / `specified-by`. A blanket
  line records the conclusion while losing the reason — the precise shape of
  silent widening that section's own last paragraph warns against.
- **`roadmap-data-model.md` § 6's Migration column is unchanged by this spec and
  is the subject of ANTS-3827.** Recorded so a reader of § 2.1.1 does not take
  this document as having settled it: this spec asserts only that no conversion
  exists *today*, which is a measurement, not a decision about what should.
- **ANTS-3756 names the wrong owner in two places, and the two amendments are
  NOT the same.** The DDL comment — *"the whole-store acyclicity check ANTS-3758
  owns"* — becomes **ANTS-3810** outright. **Its § 5 must be split rather than
  redirected**, because that bullet covers three things at once: the health-check
  suite's *"Scheduling and per-check behaviour"*, the acyclicity check, and the
  model's INV-1 second leg. Only the middle one is this spec's.
  - the scheduling / per-check sentence → **ANTS-3794**, which is what this
    spec's own § 5 already says owns cadence; redirecting it here would make
    the two documents contradict each other.
  - the acyclicity clause → **ANTS-3810**.
  - the INV-1 second-leg clause (comparing the *committed* export against a
    fresh export of the live store) → **stays unowned**, and is said to be
    unowned. It is not ANTS-3761's INV-1, not this spec's INV-1, and inventing
    an owner for it here would be the same over-broad move in the other
    direction.

  The DDL in `src/roadmapstore.cpp` carries the same `CHECK` with **no comment
  at all**, so it needs no change — the stale pointer lives only in the spec.
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
  `test_core` bundle's `SOURCES`. The directory's `spec.md` (§ 6) is a new file
  too, and is listed here because it is the artifact a build-wiring checklist
  most easily forgets — nothing in CMake references it.
- **ANTS-3794 inherits the family this spec opens**, and its bullet's claim that
  *"INV-1 already fixes the export round-trip check"* refers to **ANTS-3761's**
  INV-1, not ANTS-3758's. The two are distinguished in § 5; the bullet is
  annotated on ship so a reader of that id does not conclude the render's round
  trip is covered.

## Cold-eyes loop log

| Loop | Date | Lanes | C / H / M / L / I | Outcome |
|---|---|---|---|---|
| 3 (cap) | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; same byte-stable packet, extended again with verified source facts — no review history) | 0 / 3 / 7 / 9 / 0 | **Converged by cap. 19 verified, 19 fixed, 1 dismissed, NO deferred tail.** Origin split: **8 draft defects, 11 fix collateral** — so collateral outran draft defects a second loop running, which is Phase 5's stop trigger fully fired, and the cap is where it lands. Dimension tally: dim 2×5, dim 5×4, dim 10×3, dim 6×3, dim 7×2, dim 4×1, dim 1×1, dim 9×1. **CRITICALs reached zero and both lanes led on the same HIGH**: the fixture populated only the families that *survive* the projection, so the exclusion arms were never executed — a helper that forgot family 1 entirely, or never stripped § 2.1.1's three family-3 additions, passed every case. That is the ANTS-3797 shape this spec cites against others, one level down, and it made § 2.1.1's own `visibility` argument untested. The fixture is now a table and carries an `internal` item, a `dropped` item and a family-3 item, with INV-3 asserting each exclusion fired. Two more draft defects came from questions the lanes could not settle and verification did: `findRoadmaps()` accepts only a **root-level file case-folding to `roadmap.md`** and discovers archives **only under `docs/roadmap/`**, and section slugs are derived (`slugifyHeading()` → `uniqueSlug()`, archive-prefixed) rather than carried — three fixture preconditions each of which silently empties side B. **One finding was dismissed on verification**: a lane read the packet's roadmap enumeration as evidence that ANTS-3827 was never filed; `roadmap_query` returns it. And one defect came from neither lane — checking their legend open question against `renderLegend()` showed it walks a fixed four-status `kStatusOrder` and **skips `dropped`**, so a `dropped` legend wording is a by-design loss inside the compared set; the fixture is now scoped to the four rendered statuses. Collateral was again loop-2's own: the stage-assertion list it introduced omitted `render()`-engaged and `writeProject()`-true, so an implementer following it literally dereferences a disengaged optional on a live failure path; and its § 5.5 argument declared the id→meaning mapping protected in the same breath as narrowing it. **The trend is what closes this run: draft defects 27 → 9 → 8, falling monotonically, while collateral sat flat at 0 → 11 → 11.** The reads are still finding real things; the fixes are generating as many. That is the shape the cap exists for. Doc 807 → 905 lines. Lane spend 131k / 118k cumulative across turns. |
| 2 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; same byte-stable packet as loop 1, extended with verified source facts — no review history) | 1 / 5 / 7 / 7 / 0 | **20 verified, 20 fixed, 0 dismissed. Origin split: 9 draft defects, 11 fix collateral** — the first loop where collateral outran draft defects, and the number Phase 5's next call rests on. Dimension tally: dim 2×4, dim 7×3, dim 1×3, dim 15×3, dim 5×2, dim 10×2, dim 6×2, dim 11×1. **Both lanes independently led on the same CRITICAL, and it was a draft defect neither loop-1 lane reached**: § 7 redirected *all* of ANTS-3756 § 5 to ANTS-3810, but that bullet covers the health-check suite's scheduling, the acyclicity check **and** the model's INV-1 second leg — so executing it literally would have contradicted this spec's own § 5 (which gives scheduling to ANTS-3794) and handed this id a check it never claims. Now split three ways, with the second leg explicitly left unowned. The sharpest HIGH is a one-word one: the excluded record kind is **`rel`**, not `relationship` — `writeRelationships()` emits `{"t":"rel",…}` and `relationship` is only the table name. § 6 insists the exclusion list be *enumerated* so widening is loud; an entry keyed `relationship` matches nothing, silently, which is the exact ANTS-3797 failure the section cites. The name is wrong in ANTS-3758 § 2.6 too, so § 7 now carries the correction there. Two more draft defects: the pipeline could not be *called* as specified (`Options::liveRoadmapPath` is REQUIRED and was in none of the four call-shape rules — now five), and rule 4's "the scratch root does not leak" was verified only for `meta`, while `section.source_path` is exported, is INV-1's third break, and is safe only because `relativeSourcePath()` stores it project-relative. **The collateral was concentrated in loop 1's own additions**, and both lanes found the worst of it: loop 1 pinned INV-3 to "**each** projected `item` record" while § 2.1.2's fixture gave only the *first* item a `body` — and `body` is emitted via `insertIfPresent`, so the spec as written guaranteed a spurious RED, whose tempting repair is the projection-widening INV-3 itself forbids. Loop 1's `kMaxCyclesPerType` / `truncated` shipped with no leg reading either. Consolidation applied per 4b: the no-conversion argument (was stated 3×), the `visibility` carve-out (2×) and the `Access` rule (2×) each reduced to one home plus pointers. My own 4b sweep caught 2 further self-inflicted defects the lanes did not see — a stale "four call-shape rules" left by the fifth, and a Status line still claiming the gate had not run. Doc 716 → 807 lines. Lane spend 114k / 114k cumulative across turns. |
| 1 | 2026-08-04 | 2 (single doc, cold; genre pinned `spec`; shared byte-stable packet, ~24k of bounded windows) | 1 / 3 / 8 / 15 / 0 | **27 verified, 27 fixed, 0 dismissed, 2 re-graded.** Dimension tally: dim 6×6, dim 4×5, dim 5×4, dim 7×4, dim 1×2, dim 2×2, dim 10×2, dim 11×1, dim 13×1. All 27 are draft defects — loop 1 has no prior fixes to generate collateral. **Both lanes led on a CRITICAL and they were different ones**, which is the two-lane roll earning its cost. Lane A's survived: the header said no id blocks § 2, while § 2.1.2 named *today's code* as INV-1's red mutation — so `Inv1RoundTrip` is genuinely RED until ANTS-3808 ships and an implementer would have committed a failing case into a suite this project keeps fully green. The spec had removed the umbrella's *build-before* ordering and silently created a *commit-green-after* one; both are now stated. Lane B's was **re-graded to HIGH on verification**: it argued family 2's blanket `relationship` exclusion blinds the oracle to ~41 markdown-carried edges, correct in principle — `roadmap-data-model.md` § 6 does mark `relates-to` and `specified-by` converted — but `grep -rn 'relateItems\|relateCrossProject' src/` returns **no call site outside `roadmapstore.cpp`**, and `PlannedItem` carries no relationship field, so nothing converts them and neither side holds one. The exclusion is sound *today* and now ships with a stated trigger instead of a blanket line; the standard-vs-code divergence is filed as ANTS-3827. The other HIGHs were both real contract gaps: the **comparison relation was never defined** (byte-vs-multiset, line-vs-key — and only the byte/sequence reading catches the element-ordering break INV-1 names), and § 7 told an implementer both to annotate and to edit ANTS-3758's INV-1. Also fixed: cycle rotation, multiplicity and bound were all unpinned while INV-2/INV-4 asserted on exact path output; `unresolvedEdges` had a second dangling shape in neither bucket. Two verified-WRONG author claims died here — the `source` vs `source_path` "inconsistency" is the export's own deliberate key naming (its emitter says so in a comment), and `bug → fix` is not in `mappedKind()`; `bugfix → fix` is. Doc grew 543 → 716 lines. Lane spend 107k / 107k cumulative across turns, ~58k on the first turn against the 60k per-turn budget. |
| 0-split | 2026-08-04 | 0 (no reviewer dispatched — a document operation) | — | **Split from the 934-line umbrella `docs/specs/ANTS-3793-roadmap-consumer-cutover.md`, carrying its §§ 2.6–2.7 and INV-7 / INV-8.** Not a review loop and not inherited review: the umbrella's three loops ran against a document that no longer exists, so the gate runs from loop 1 on these bytes. The umbrella carried seven contracts and stopped at `/cold-eyes`' cap with collateral outnumbering draft defects two loops running; the user split it four ways on 2026-08-03 (ANTS-3793 read seam, ANTS-3808 `item.body`, ANTS-3809 write half, ANTS-3810 this). Invariants renumbered from 1 with the mapping in § 3. Drafting also changed three inherited claims against source rather than carrying them: § 2.2's signature dropped its `projectId` (the model's § 6 scopes acyclicity to the full store), § 2.1.1 added three fields to ANTS-3758 § 2.6's projection, and § 2.1.2 replaced the umbrella's cross-id build-order constraint with the mutation harness. |
