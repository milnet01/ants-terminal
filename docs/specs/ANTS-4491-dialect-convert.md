# ANTS-4491 — Convert a github-task-list roadmap to canonical ants-v1

**Status:** accepted, cold-eyes loops 1 + 2 folded (2026-09-20).
**Kind:** implement.
**Source:** ROADMAP ANTS-4491 (cc-feedback-2026-08-18, Vestige).

**Blocked by:** ANTS-4500.
**Pairs with:** ANTS-4485, ANTS-4487.

**Layman:** Give a project still using the old checklist-style roadmap a
single command that converts it to the standard format, instead of asking
someone to rewrite every line by hand.

## 1. Goal

One operation migrates a `github-task-list` project, records its dialect as
`ants-v1`, and rewrites its `ROADMAP.md` in canonical ants-v1 — atomically,
so no reader ever observes the store and the file disagreeing. Running it
twice changes nothing the first run did not.

## 2. Problem

Vestige cannot adopt the standard roadmap format. A hand conversion would
rewrite its whole roadmap in one commit and allocate an id for every bullet
that lacks one, and the project holds a standing decision not to do that by
hand. So the conversion belongs in the tool.

Three properties of the real input shape the work, and none is in the
original ask.

**The source is already part-converted, and the converted fraction grows.**
`roadmap_log op:"append"` writes ants-v1 whatever dialect the file is in
(`roadmap-format.md` § 3.10.2), so any project that has filed work through
the verb is mixed. Measured against Vestige's `ROADMAP.md`:

```
grep -cE '^\s*- \[[ xX]\]' ROADMAP.md                                  # 994
grep -cE '^\s*- (📋|🚧|✅|💭|🚫)' ROADMAP.md                            # 106
grep -cE '^\s*- \[[ xX]\] \[[A-Za-z0-9_]+-[0-9]+\]' ROADMAP.md         # 0
```

The ants-v1 population was recorded as 45 when this item was filed and is now
106. A converter assuming a uniform source will skip those bullets or convert
them twice.

The mixing is append-side only: a flip applies per the bullet's own format, so
the already-converted population is exactly the set carrying bracket ids. That
gives a per-bullet test rather than a heuristic.

**The two populations do not share an id scheme.** No GFM bullet carries a
bracket id; every bracket id sits on an emoji bullet. The GFM bullets carry
prose lead-ins, which ANTS-3771 lets a project resolve by declaring its own id
grammar and ANTS-4575 lets a reader flag as inferred.

**The store is a stale mirror, so it is not a safe id source alone.**

```
sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite \
  "SELECT MAX(CAST(substr(id,6) AS INTEGER)) FROM item
   WHERE project_id=(SELECT project_id FROM project WHERE root LIKE '%Vestige')
     AND id GLOB '3D_E-[0-9]*';"                            # 612
grep -oE '3D_E-[0-9]+' ROADMAP.md | sed 's/3D_E-//' | sort -n | tail -1   # 682
```

The gap was 612 against 624 when this item was last measured and is now 612
against 682. It widens as the project files work the store has not seen.

## 3. Scope decisions (agreed with the user)

None specific to this spec. Its one inherited decision is ANTS-4500 § 3's:
synthesis is marked and reserved going forward, and existing synthesised ids
are not renamed. That is what makes rendering this project's ids safe — see
§ 4.5.

## 4. Surface

### 4.1 The emitter already exists

`RoadmapRender::render()` writes every **renderable** item in canonical
bullet form and selects its dialect from `Options::dialect`. Renderable
excludes `visibility == "internal"`, which is its only per-item filter. So this is not "write an ants-v1
emitter". The work is what the store holds when the render runs, and the
ordering around it.

### 4.2 Neither single-step order is safe

`RoadmapSource::migratedProject()` dispatches on the **live file's** detected
format, from `RoadmapParse::detectRoadmapFormat()`. The stored `source_format`
is a second witness, used only to refuse a disagreement, and that refusal is
checked before the dialect gate. So:

| Step taken alone | Resulting state | Effect |
|---|---|---|
| Rewrite the file first | file ants-v1, store github-task-list | every read refuses `ReadError::SourceUnrecognised` |
| Set `source_format` first | file github-task-list, store ants-v1 | every read refuses `ReadError::SourceUnrecognised` |

Both brick the project. The two writes must be atomic with respect to any
reader.

### 4.3 Ride `commitAndRender()`

`RoadmapWrite::commitAndRender()` already solves that shape: pre-image, begin,
`mutate()`, dry render, divergence guard, commit, publish.

**What the convert's `mutate()` does, in order** — stated because a `mutate()`
that only writes the column is a different operation from one that reconciles
ids, and § 4.6 depends on the second:

1. Parse the live file and build the plan, as a migration does.
2. Re-match each parsed bullet to its stored item, including **ANTS-4500
   § 4.5's re-match fallback for an id present in the file and absent from
   the store**. That is a stale-mirror re-match on an id-bearing bullet, not
   ANTS-3765 § 2.6.1's id-less key; conflating them matches a file-only id by
   headline and inserts a duplicate when the headline has since been edited.
   On such a match the file's id replaces the stored one, per ANTS-4500
   § 4.5.
3. Allocate an id for each bullet still without one, against § 4.6's floor.
4. Set the project's `source_format` to `ants-v1`.

Step 4 alone would leave the store holding whatever an earlier migration left
and the render publishing that, which is not a conversion of the file in front
of it.

This works because `opts.dialect` is read from the store *after* `mutate()`
has run, so the validating dry render and the publish both emit ants-v1 within
the one sequence. No bespoke ordering is written.

Its one declared window — store committed, file not yet published — is worse
here than on an ordinary write. There it leaves a stale but readable file;
here it leaves the pair disagreeing, so reads refuse. The remedy already
exists and the refusal already names it: re-running the migration records the
file's format and clears the disagreement. § 5 pins that as an invariant.

### 4.4 The accepted set, and idempotence per bullet

**The convert accepts a source that reads as `github-task-list` or as
`ants-v1`, and refuses anything else.** Accepting `ants-v1` is not an
oversight: after a successful run the file *is* ants-v1, so refusing that
dialect would make the operation refuse its own output and INV-3
unsatisfiable. On an already-`ants-v1` source the convert is a plain render
and changes nothing.

**Two refusals, split by what `detectRoadmapFormat()` returns.** It answers
exactly `ants-v1`, `pass-headings` or `github-task-list`, and its no-signal
default is `ants-v1`. So:

- `pass-headings` — a *recognised* third dialect — refuses
  `dialect_out_of_scope`. `migratedProject()` does not refuse it, which is
  why this spec needs its own code.
- A source with **no format signal** takes `migratedProject()`'s existing
  `ReadError::SourceUnrecognised`. The convert never runs.

The per-bullet question arises on the *first* run, over a mixed source: a
bullet already carrying a bracket id is already converted and keeps its id
rather than being allocated a second one.

### 4.5 What the render stamps into the file

Rendering writes every renderable item's id in brackets, including
synthesised ones. An `internal`-visibility item is not rendered, so its id is
not settled by this run and stays positional. Under
ANTS-4500 § 3 decision 1 the already-synthesised ids are not renamed, so this
run stamps them in their present form.

That is the correct outcome rather than a hazard. An invented id is unstable
only because allocation is positional across re-migrations; once rendered into
the file it parses as `parsed` on the next migration and ANTS-3765 § 2.6's
**id match** holds the item — § 2.6.1's weaker key is for an id-less item and
is no longer needed once the id is in the file. **The render is what settles them.** The ordering
requirement that follows is § 5's INV-4: **no re-migration may intervene
between the allocation and the commit**, which riding `commitAndRender()`
guarantees, both happening inside its one transaction.

The commit-to-publish window of § 4.3 is *outside* that guarantee and nothing
narrows it. What it costs is bounded: the ids are committed, so a re-migration
there re-matches them rather than re-allocating, and the remedy for the
unpublished file is the same re-run the refusal already names.

### 4.6 Id reconciliation

**`RoadmapStore::allocationFloor()` alone is not a safe floor here, and this is
the trap.** Both its terms are store-side — `idHighWater()` reads the
`id_prefix` counter, `maxAllocatedId()` reads ids off `item` rows. Neither can
see an id that is live in the file and absent from the store, which is exactly
the staleness § 2 measures: the floor would be 612 while `3D_E-0613` through
`3D_E-0682` are already in the source.

So the floor is the higher of `allocationFloor(projectId, prefix)` **and the
maximum numeric suffix among the file's parsed ids carrying that same
prefix** — the prefix resolved per ANTS-3765 § 2.8 step 1, ids whose suffix
does not parse as an integer ignored, and quarantined and synthesised ids
excluded from both terms. The prefix scoping is load-bearing: an unscoped
`MAX` lets one foreign-prefix bullet set the floor, which is how
`maxAllocatedId()`'s own comment records one absurd sample id burning ~5,370
numbers. § 2.8 step 2's amendment says the rest outright: neither term
dominates the other, so neither can be the one that wins.

Matching is the second half. An id live in the file but absent from the store
must match its item rather than produce a fresh insert. That is ANTS-4500
§ 4.5's re-match fallback, and this spec depends on it.

## 5. Invariants

- **INV-1** — After a successful convert, the file's detected format and the
  stored `source_format` agree.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `formatsAgree` — convert a github-task-list fixture, then assert
  `detectRoadmapFormat()` over the written file returns `ants-v1` and the
  stored `source_format` is `ants-v1`.
  *Breaks when:* either write lands without the other.

- **INV-2** — A convert that fails at any point before commit leaves both the
  store and the file exactly as they were.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `failedConvertIsInert` — force the dry render to fail, assert the file's
  bytes and the stored `source_format` are unchanged.
  *Breaks when:* the convert writes the file outside `commitAndRender()`.

- **INV-3** — Converting twice produces the same file as converting once.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `convertIsIdempotent` — convert, hash the file, convert again, assert the
  hash is unchanged.
  *Breaks when:* a bullet already carrying a bracket id is allocated a second
  id.

- **INV-4** — No id changes between the allocation that assigns it and the
  commit that persists it.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `idsStableAcrossCommit` — force a failure after allocation and before
  commit, then assert no `item` row and no raised `id_prefix.high_water`
  survive. Asserting that published ids match stored ids cannot fail: the file
  is rendered *from* the store, so its ids are a projection of the store's
  whatever happened at the boundary.
  *Breaks when:* the convert allocates in one transaction and commits in
  another, so an aborted run leaves an id burnt.

- **INV-5** — A bullet that already carries a bracket id keeps that id.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `existingIdsPreserved` — convert a mixed fixture, assert every pre-existing
  bracket id appears unchanged in the output.
  *Breaks when:* the convert treats the source as uniformly id-less.

- **INV-6** — An id present in the file but absent from the store matches its
  existing item rather than creating a second one.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `staleMirrorDoesNotDuplicate` — seed a store behind its file, convert,
  assert the item count equals the file's bullet count, counting renderable
  items only. An `internal`-visibility item is in the store and not the file,
  so a plain equality fails for a reason unrelated to duplication.
  *Breaks when:* ANTS-4500 § 4.5's re-match fallback is absent.

- **INV-7** — A source `detectRoadmapFormat()` recognises as a third dialect
  refuses `dialect_out_of_scope` and writes nothing.
  *Test:* `tests/features/roadmap_convert/test_convert.cpp`, case
  `thirdDialectRefused` — point the convert at a pass-headings fixture, assert
  the refusal code and that the file is unchanged. The fixture must carry two
  `Pass` headings and two `Status` markers and no emoji bullets, or
  `detectRoadmapFormat()` classifies it `ants-v1` and the case tests the
  accepted path instead.
  *Breaks when:* the convert relies on `migratedProject()`, which recognises
  that dialect and does not refuse it.

## 6. Failure modes

**The layman gate refuses the publish.** `commitAndRender()` scopes that gate
to the items the write touched, taken from `itemsWrittenSinceBegin()`. A
convert touches every item, so on a project carrying legacy debt the gate can
still refuse the whole run. This is the one failure mode that makes the
operation unavailable rather than merely aborted, and it is handled in § 9 as
out of scope with a named owner, not silently.

**The project is not registered.** The convert refuses rather than migrating
implicitly. Migration is a separate operation with its own guards.

**The source has no format signal.** `migratedProject()` already refuses
`ReadError::SourceUnrecognised` and the convert does not run. A *recognised*
third dialect is the other branch and refuses `dialect_out_of_scope` — see
§ 4.4, which owns the split.

**Publish fails after commit.** The declared window of § 4.3. The store is
ants-v1 and the file is not, so reads refuse with the message that names the
remedy. Recovery is re-running the migration; nothing is lost.

## 7. Tests

All cases live in `tests/features/roadmap_convert/`, paired with `spec.md`,
compiled into an existing bundle's `SOURCES` list. `build_target_for` names
the bundle. Each is seen to fail against pre-change code before the change is
restored.

| Invariant | Case |
|---|---|
| INV-1 | `formatsAgree` |
| INV-2 | `failedConvertIsInert` |
| INV-3 | `convertIsIdempotent` |
| INV-4 | `idsStableAcrossCommit` |
| INV-5 | `existingIdsPreserved` |
| INV-6 | `staleMirrorDoesNotDuplicate` |
| INV-7 | `thirdDialectRefused` |

Fixtures are throwaway stores under the test's own temp root, never the
machine-global store. `RoadmapStore`'s default path is the real store, so each
case passes an explicit path.

Label: `features`.

## 8. Alternatives considered (and rejected)

**A bespoke two-phase sequence.** Rejected: `commitAndRender()` already
implements the atomicity this needs, and a second sequence would be a second
place for the ordering of § 4.2 to be got wrong.

**Convert by rewriting the file and re-migrating.** Rejected: that is the hand
conversion this item exists to replace, and it passes through the bricked
state of § 4.2 on the way.

**Renaming synthesised ids during the convert.** Rejected by ANTS-4500 § 3
decision 1, and unnecessary — § 4.5 explains why rendering settles them.

## 9. Out of scope

- A route to the `layman` column that is not a hand edit plus a re-import —
  tracked by ANTS-4434. Until it exists, a project carrying many open items
  with an empty `layman` column cannot complete a convert. This is the live
  constraint on Vestige specifically.
- Writing allocated ids back into a source file that is not being converted —
  tracked by ANTS-3758.
- Converting a recognised dialect that is neither `github-task-list` nor
  `ants-v1`.
  Those refuse `dialect_out_of_scope`; see § 4.4 for why `ants-v1` is
  accepted rather than refused.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_convert.cpp::formatsAgree` |
| INV-2 | `test_convert.cpp::failedConvertIsInert` |
| INV-3 | `test_convert.cpp::convertIsIdempotent` |
| INV-4 | `test_convert.cpp::idsStableAcrossCommit` |
| INV-5 | `test_convert.cpp::existingIdsPreserved` |
| INV-6 | `test_convert.cpp::staleMirrorDoesNotDuplicate` |
| A third dialect refuses `dialect_out_of_scope` | `test_convert.cpp::thirdDialectRefused` |

## 11. Cross-doc impact

- `docs/standards/roadmap-format.md` — § 3.10.2 gains the convert as the
  supported route out of a mixed file.
- `docs/standards/mcp-error-codes.md` — `dialect_out_of_scope` is a new
  refusal code.
- `CHANGELOG.md` — a bullet stating what shipped.
- `ROADMAP.md` — ANTS-4491 flips when this ships; ANTS-4434 remains open and
  is what gates Vestige's own conversion.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4491-dialect-convert-loop-log.md`.
