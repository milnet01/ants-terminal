# ANTS-4500 — Give a synthesised id its own namespace and its own counter

**Status:** accepted, cold-eyes loops 1 + 2 folded (2026-09-20).
**Kind:** implement.
**Source:** ROADMAP ANTS-4500 (cc-feedback-2026-08-18, Vestige; split from ANTS-4493).

**Blocker for:** ANTS-4491.
**Pairs with:** ANTS-4485, ANTS-4487.

**Layman:** When the migration invents a number for a roadmap item that has
none, that number will look different from a real one, and it will stop using
up the numbers real items are waiting for.

## 1. Goal

A migration that invents an id for an id-less bullet renders it as
`<prefix>-S<NNNN>` and draws it from a counter reserved for synthesis. A
reader can tell an invented id from an allocated one at sight. Inventing one
no longer advances the counter that real items allocate from. Items that
already carry an invented id keep it.

## 2. Problem

`Loader::allocateId()` in `src/roadmapmigrateload.cpp` issues the next value
in the project's own counter space for a bullet that carries no id. Two
consequences.

1. An invented id is indistinguishable from an allocated one once written, and
   it is positional — it depends on where the bullet sits and on what the plan
   carried that run. ANTS-4493 records one moving: an id synthesised onto an
   audio bullet was later allocated to a real item, and a re-migration resolved
   the clash by moving the audio bullet. So an invented id cannot be cited in a
   commit message, a spec or another item.
2. Synthesis spends the live id space. Every invented id advances the counter
   that `roadmap_log op:"append"` allocates from, so numbers nobody chose are
   consumed.

The collision half is already fixed: `RoadmapStore::allocationFloor()` floors
the markdown allocator to the store, so the two allocators cannot issue the
same number. What remains is citability and the spend.

The populations that make this worth fixing, measured against the live store:

```
sqlite3 -readonly ~/.local/share/ants-terminal/roadmap.sqlite \
  "SELECT p.root, COUNT(*), SUM(CASE WHEN i.id_origin='synthesised'
   THEN 1 ELSE 0 END) FROM item i JOIN project p ON p.project_id=i.project_id
   GROUP BY p.root ORDER BY COUNT(*) DESC;"
```

Ants_Terminal 1041 of 2881 items are synthesised; Vestige 567 of 1026; and
UT_Ants, Pressless and demoreel are synthesised in full.

## 3. Scope decisions (agreed with the user)

Taken 2026-09-20, both recorded on ANTS-4500.

1. **New synthesis only.** Rows already holding an invented id keep it; no
   retroactive rename. Three projects are synthesised in full, and relabelling
   every id in them as non-citable would be false — an id that has been
   rendered into the file parses as `parsed` on the next migration, and
   ANTS-3765 § 2.6.1's key holds the item, so those ids have settled.
2. **The id validators are widened to accept the new shape.** A synthesised
   item stays reachable by `flip`, `annotate` and an id fetch. It is visibly
   non-quotable to a reader without becoming unreachable to tooling.

Option 2 of the four the reporter listed was chosen. § 8 records why the other
three lost.

## 4. Surface

### 4.1 The rendered form

`Loader::allocateId()` renders a synthesised id as the chosen prefix, `-S`,
and the suffix zero-padded to four digits, widening past four as the counter
passes them. This mirrors the allocated form ANTS-3765 § 2.8 step 3 pins.

```
ANTS-S0001      first synthesised id for prefix ANTS
3D_E-S0042
ANTS-S12345     once the synthesis counter passes four digits
```

An allocated id is unchanged: `<prefix>-<NNNN>`.

### 4.2 The synthesis counter

**The counter lives in `id_prefix` under a key no declared prefix can
collide with: `<prefix>#S`.** A declared prefix is
`[A-Za-z][A-Za-z0-9_-]{0,63}` (`src/main.cpp`'s own message states the
charset), so `#` cannot appear in one. An earlier draft keyed the row on
`<prefix>-S` and excluded it by suffix test; that broke a project whose
declared prefix legitimately ends `-S`, hiding its *real* counter. The key is
an internal detail and does not appear in any rendered id.

The table is keyed `(project_id, prefix)`, so this is a second row rather than
a new column, and no `kSchemaVersion` bump is involved.

**The floor for a synthesis allocation is three terms.** ANTS-3765 § 2.8
step 2's 2026-08-13 amendment exists because a single term was measured wrong
on this project, and it is explicit that neither of its terms dominates. The
synthesis namespace has the same exposure plus one more, because its ids can
reach the file and come back:

1. the stored `<prefix>#S` counter row, via `RoadmapStore::idHighWater()`;
2. the maximum `S`-suffix among the project's **stored** `<prefix>-S<digits>`
   ids; and
3. the maximum `S`-suffix among the **plan's parsed** `<prefix>-S<digits>`
   ids — the file-side term.

**Term 3 is the one that covers a store restored behind its file.** Terms 1
and 2 both read the store, so a restore that lost the `item` rows loses both;
only the plan sees what the file already holds. An earlier draft justified a
two-term floor with exactly that case and did not cover it.

**Term 2 needs a new accessor.** `RoadmapStore::maxAllocatedId()` cannot
express it: it globs `<prefix>-[0-9]*`, so it matches no `-S` id at all. It
gains a sibling — `maxSynthesisedId(projectId, prefix)` — globbing
`<prefix>-S[0-9]*` and casting the suffix past `<prefix>-S`. A member rather
than a migration-local helper, for the reason `allocationFloor()` is one: term
3's caller is the loader and term 2's is anything that needs the floor.

**Migration also ensures the real prefix's `id_prefix` row exists.** Without
it a wholly-synthesised project would carry only a `#S` row, and
`RoadmapStore::idPrefixFor()` — which picks one row per project and is reached
by `Loader::allocateId()`, `roadmap_log`'s append allocator and
`RoadmapFoldIn` — would return the synthesis key or nothing. Fold-in gives up
on an empty result and the append allocator falls through to a directory-leaf
guess, which is two id families in one store. Three corpus projects are
synthesised in full, so this is the expected case there.

**`idPrefixFor()` ignores any row whose prefix contains `#`.** A structural
test, not a suffix one: it cannot mistake a legal declared prefix for a
counter key.

`RoadmapStore::maxAllocatedId()` needs no change for the *real* prefix: it
globs `<prefix>-[0-9]*`, and `ANTS-S0001` does not match, so a synthesised id
already drops out of the real prefix's floor.

### 4.3 The high-water terms ANTS-3765 § 2.8 step 2 defines

Step 2 excludes synthesised ids from its **plan-side** term by origin. The
store-side term is not an origin filter at all — `idHighWater()` reads a
counter column and cannot see `id_origin` — and what keeps a synthesised id
out of the real prefix's store-side floor is its *shape*, per § 4.2. The plan-side term additionally ignores any id whose suffix does
not parse as an integer, which covers a re-migration meeting an `-S` id that
was rendered into the file by an earlier run.

### 4.4 Widening the id grammar

**The surface that decides addressability is the parser's id grammar, not the
refusal helpers.** `RoadmapParse::idTokenPattern()` returns
`(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+`, and
`RoadmapIndex::isCanonicalId()` anchors the same shape. A token failing it is
not parsed as a project id at all — the bullet is assigned a synthetic
content-hash id, and the authored token is unaddressable on both the read
(`id`/`ids`) and write (`flip`/`annotate`) locator paths. That is what INV-4
is about, and widening it is what makes INV-4 pass.

| Symbol | File | Role |
|---|---|---|
| `RoadmapParse::idTokenPattern` | `src/roadmapparse.cpp` | **the addressability gate** — widen |
| `RoadmapIndex::isCanonicalId` | `src/roadmapindex.cpp` | the canonical-id predicate — widen |
| `rcIsNonconformingIdToken` | `src/remotecontrol.cpp` | diagnostic only — see below |
| `looksLikeRoadmapId` | `src/findsources.cpp` | recognises a roadmap id in prose — widen |
| `rcdetail::rcRoadmapIdLess` | `src/remotecontrol_terminal.cpp` | ordering — widen |
| `rxAntsV1IdBracket` | `src/remotecontrol.cpp` | **the WRITE path's id bracket** — widen |

The five rows marked *widen* admit the `-S` infix. `rcIsNonconformingIdToken` is left unchanged — neither of its regexes is touched, for the reason below.

**`rxAntsV1IdBracket` was added 2026-09-21, by implementation.** The table
above called `idTokenPattern()` *the* addressability gate, and it gates only the
READ half. `roadmap_log`'s write locators parse the file with
`walkAntsV1Bullets()`, which carries its own bracket regex, so INV-4 failed with
the read resolving `DEMO-S0001` and `op:"annotate"` refusing `bullet_not_found`
against the same id. That symbol's own history is this divergence recurring:
ANTS-2051 taught it lowercase prefixes and ANTS-4109 the bold-ID form, both
reported as "roadmap_query resolves it and roadmap_log matches zero bullets".
Widen the write path whenever the read path widens.

**`rcIsNonconformingIdToken` is a diagnostic and resolves nothing.** It
returns true only for a token that is id-*ish* but non-canonical, so callers
can emit a targeted `bad_id_format` naming the real cause. Its own comment
warns against widening `kIdIsh`, because making both regexes admit the same
shape collapses the guard to `X && !X`. So its row here is about which error
text a caller emits, never about whether a synthesised id can be reached — an
earlier draft of this spec had that backwards, and widening it alone would
have left INV-4 failing.

`rcRoadmapIdLess` degrades rather than fails: an unparseable suffix falls back
to a lexicographic compare on the full id. It is widened so synthesised ids
order by their own number once the counter passes four digits, where a text
compare would put `<prefix>-S10000` before `<prefix>-S9999`.

**`RoadmapIndex::isCanonicalId` has callers that are not locators** — the
roadmap dialog's duplicate-id banner, the feedback verbs' id filter, and the
MCP duplicate detector. Widening it admits synthesised ids to all of them,
which is correct: they are ids, and a duplicate among them is still a
duplicate.

### 4.5 The § 2.6 re-match companion

Not optional. A stored row holds `ANTS-S0001` while a person filing that item
by hand writes a conventional id, so ANTS-3765 § 2.6's id match fails and the
migration produces a fresh insert plus an orphan. ANTS-4343 is evidence that
path is used.

So § 2.6 is amended: an id-bearing item that finds no id match falls back to
§ 2.6.1's id-less key before being treated as new. This spec carries the
amendment; ANTS-3765 § 2.6 is edited in the same change.

**The source's id wins.** On a match through that fallback the stored row
takes the id the source file carries, replacing the synthesised one. The
alternative — keeping the stored `<prefix>-S<NNNN>` — makes the next render
overwrite the author's hand-written id in `ROADMAP.md`, which is the file
losing an authored edit. Nothing cites a synthesised id by contract, because
§ 4.1's whole purpose is to mark it as not a citation target, so replacing it
costs nothing that the stored id was carrying. `element` and `history` rows
key on `item_pk`, not on the id string, so they follow the row.

## 5. Invariants

- **INV-1** — A migration that invents an id renders it with the `-S` infix.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `rendersSSuffix` — migrate a plan holding one id-less bullet, assert the
  stored id matches `^[A-Za-z0-9_-]+-S\d{4,}$`.
  *Breaks when:* `allocateId()` renders the allocated form for an id-less
  bullet.

- **INV-2** — Inventing an id does not advance the real prefix's counter.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `realCounterUntouched` — record `idHighWater(pid, "ANTS")`, migrate a plan
  of id-less bullets, assert it is unchanged.
  *Breaks when:* allocation passes `<prefix>` rather than `<prefix>-S` to
  `raiseIdHighWater()`.

- **INV-3** — Two synthesised ids in one project never collide.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `synthIdsUnique` — migrate a plan of several id-less bullets, clear the
  `<prefix>#S` counter row, then re-migrate a source carrying **additional
  id-less bullets with new headlines**, and assert every stored id is
  distinct. The new headlines are what make this falsifiable: re-migrating an
  unchanged source re-matches every bullet by ANTS-3765 § 2.6.1's key and
  allocates nothing, so the floor is never consulted and a one-term floor
  passes. A third leg clears the `item` rows too and leaves an `-S` id in the
  source, which only term 3 can see.
  *Breaks when:* the synthesis floor omits term 2 or term 3 of § 4.2.

- **INV-4** — A synthesised id is reachable by every locating read and write.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `synthIdAddressable` — fetch a synthesised id, then `op:"annotate"` it, and
  assert neither refuses.
  *Breaks when:* a validator in § 4.4 is left requiring a digit suffix.

- **INV-5** — An item already holding a synthesised id keeps that id across a
  re-migration **of an unchanged, still-id-less source bullet**. § 4.5's
  re-match fallback is the one case that replaces it, and INV-6 tests that.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `existingIdsUnchanged` — migrate, record the ids, re-migrate the same source,
  assert every id is unchanged.
  *Breaks when:* the change is applied retroactively, against § 3 decision 1.

- **INV-6** — An id-bearing item whose id matches no stored id is matched on
  § 2.6.1's id-less key before being treated as new.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `reMatchFallsBackToKey` — migrate an id-less bullet, rewrite the source
  bullet with a hand-written id and the same headline, re-migrate, assert the
  item count is unchanged, no orphan is reported, **and the row now carries
  the source's id rather than the synthesised one**.
  *Breaks when:* § 2.6 treats an id miss as a new item, or the fallback keeps
  the stored id and the next render overwrites the author's.

- **INV-7** — A synthesised id never becomes the project's allocation prefix.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `synthPrefixNotProjectPrefix` — build a project whose synthesis counter
  exceeds its real one, then allocate through `roadmap_log op:"append"` and
  assert the new id is `<prefix>-<NNNN>`, not `<prefix>-S-<NNNN>`.
  *Breaks when:* `idPrefixFor()` does not ignore a prefix containing `#`; its
  `ORDER BY high_water DESC` then returns the synthesis row.

- **INV-8** — A synthesised id is addressable by the parser, not merely by a
  better error message.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `synthIdParsesAsId` — assert `RoadmapIndex::isCanonicalId("ANTS-S0001")` is
  true and that parsing a bullet carrying that id yields it rather than a
  synthetic content-hash id.
  *Breaks when:* only the refusal helpers are widened and the parser's id
  grammar is left narrow.

- **INV-9** — A wholly-synthesised project still resolves its real prefix.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `realPrefixRowEnsured` — migrate a project whose every bullet is id-less,
  then assert `idPrefixFor()` returns the real prefix and not the counter key,
  and that `roadmap_log op:"append"` allocates `<prefix>-<NNNN>`.
  *Breaks when:* migration writes only the `#S` row, so `idPrefixFor()` finds
  nothing and the allocator falls through to a directory-leaf guess.

## 6. Failure modes

**The prefix itself ends in `-S`.** A project whose declared prefix is `FOO-S`
gets a counter row at `FOO-S#S` and renders `FOO-S-S0001`. Ugly, and correct:
the `#` key means the project's real `FOO-S` row is still what
`idPrefixFor()` returns. This is why § 4.2 keys on `#` rather than testing for
a trailing `-S`, which would have hidden that project's real counter.

**A source file already contains an `-S`-shaped id.** The corpus sweep found
none, so this is a future state rather than a present one. Such an id parses
as `parsed`, and § 2.8 step 2's plan-side term ignores it because its suffix
is not an integer. It therefore raises neither counter. It is matched by id
like any other parsed id.

**The synthesis counter row is missing.** `idHighWater()` returns `nullopt`,
and a missing row does *not* mean the project has never synthesised — a store
restored from a backup reports the same thing while `-S` ids are live in the
source. So allocation does not start at 1 on that evidence alone: term 2 reads
the ids the items hold and term 3 reads the ids the file holds, and only when
all three are empty does the first allocation take `S0001`. ANTS-3765
§ 2.8 step 4's start-at-zero condition is about the *plan* carrying no parsed
integer-suffix id, not about a stored row being absent.

**A rolled-back load.** Allocation stays inside the write transaction, so a
failed run advances neither counter. This is ANTS-3765 § 2.8's existing rule
and is not changed here.

## 7. Tests

All cases live in `tests/features/roadmap_synth_id/`, paired with
`spec.md`, and are compiled into an existing bundle's `SOURCES` list rather
than added as a standalone executable. `build_target_for` names the bundle.

| Invariant | Case |
|---|---|
| INV-1 | `rendersSSuffix` |
| INV-2 | `realCounterUntouched` |
| INV-3 | `synthIdsUnique` |
| INV-4 | `synthIdAddressable` |
| INV-5 | `existingIdsUnchanged` |
| INV-6 | `reMatchFallsBackToKey` |
| INV-7 | `synthPrefixNotProjectPrefix` |
| INV-9 | `realPrefixRowEnsured` |
| INV-8 | `synthIdParsesAsId` |

Each case must be seen to fail against pre-change code before the change is
restored. INV-5's case is the exception in kind: it passes before the change
and must keep passing after, so it is run in both states and is a regression
guard rather than a red-first test.

Label: `features`.

## 8. Alternatives considered (and rejected)

**Option 1 — reserve above the counter's high-water.** Leaves a synthesised
id numerically indistinguishable from a real one, so a reader still cannot
tell which ids are quotable. It stops collisions, which `allocationFloor()`
already stops.

**Option 3 — a separate `synthetic_id` column, leaving `id` NULL.** The
cleanest model and the largest change. It requires a `kSchemaVersion` bump,
which is a one-way door: the store is machine-global and `RoadmapStore::open()`
refuses when the store's `user_version` exceeds the build's, so the first
binary to upgrade locks every older build out of every project. Rejected as
disproportionate to a problem the id string solves.

**Option 4 — keep sharing the space and bump `.roadmap-counter` past the
high-water.** Same defect as option 1: the id stays indistinguishable. It also
does not stop the spend, only reports it.

**Retroactive renaming of existing synthesised ids.** Rejected by the user —
see § 3 decision 1.

**Leaving the validators narrow, so a synthesised id is unaddressable.**
Rejected by the user. It enforces "never cite this" hard, at the cost that a
synthesised item cannot be flipped or annotated at all.

## 9. Out of scope

- Writing allocated ids back into the roadmap source file — the durable fix
  for id instability, named by ANTS-3765 § 2.6.1 and tracked by ANTS-3758.
- The orphan produced when a re-match fails for reasons other than an id miss
  — tracked by ANTS-4487.
- `id_origin` remaining `synthesised` after the source file starts declaring
  the id — tracked by ANTS-4343.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_synth_id.cpp::rendersSSuffix` |
| INV-2 | `test_synth_id.cpp::realCounterUntouched` |
| INV-3 | `test_synth_id.cpp::synthIdsUnique` |
| INV-4 | `test_synth_id.cpp::synthIdAddressable` |
| INV-5 | `test_synth_id.cpp::existingIdsUnchanged` |
| INV-6 | `test_synth_id.cpp::reMatchFallsBackToKey` |
| INV-7 | `test_synth_id.cpp::synthPrefixNotProjectPrefix` |
| INV-9 | `test_synth_id.cpp::realPrefixRowEnsured` |
| INV-8 | `test_synth_id.cpp::synthIdParsesAsId` |
| § 4.1's rendered form is stable once shipped | **nothing** — no check can see a future change of mind; ANTS-3765 § 2.8 step 3 states the permanence |

## 11. Cross-doc impact

- `docs/specs/ANTS-3765-roadmap-migration-load.md` — § 2.8 gains the synthesis
  namespace and its counter; § 2.6 gains the id-less-key fallback of § 4.5.
- `docs/standards/roadmap-format.md` — § 3.5.1's id grammar admits the `-S`
  infix, and § 3.10.4's prefix conventions gain the `-S` exclusion of § 4.2.
  This project is upstream of the global copy, so the global copy is
  corrected to match.
- `CHANGELOG.md` — a bullet stating what shipped, authored rather than copied
  from this item's headline, which states a problem.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4500-synthesised-id-namespace-loop-log.md`.
