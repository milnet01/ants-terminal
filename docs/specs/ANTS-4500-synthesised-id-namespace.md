# ANTS-4500 — Give a synthesised id its own namespace and its own counter

**Status:** spec draft (2026-09-20).
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

The synthesis counter is an `id_prefix` row whose `prefix` column holds
`<prefix>-S`. The table is keyed `(project_id, prefix)`, so this is a second
row rather than a new column, and no `kSchemaVersion` bump is involved.

Allocation reads and advances that row through the existing
`RoadmapStore::idHighWater()` and `raiseIdHighWater()`, passing `<prefix>-S`
where it passes `<prefix>` today. The real prefix's counter is not read and
not advanced.

`RoadmapStore::maxAllocatedId()` needs no change. It selects on
`id GLOB '<prefix>-[0-9]*'`, and `ANTS-S0001` does not match that pattern, so
a synthesised id already drops out of the floor computed for the real prefix.

### 4.3 The high-water terms ANTS-3765 § 2.8 step 2 defines

Step 2 already excludes synthesised ids from both terms by origin. That
exclusion stands and is what keeps a synthesised id out of the real prefix's
high-water. The plan-side term additionally ignores any id whose suffix does
not parse as an integer, which covers a re-migration meeting an `-S` id that
was rendered into the file by an earlier run.

### 4.4 Widening the id validators

Three surfaces reject an id whose suffix is not digits. Each gains the
optional `S`.

| Symbol | File | Present shape |
|---|---|---|
| `rcIsNonconformingIdToken` | `src/remotecontrol.cpp` | `kIdIsh` and `kCanonical` both end `-\d+$` |
| `looksLikeRoadmapId` | `src/findsources.cpp` | `^[A-Z]{2,8}-\d+$` |
| `rcdetail::rcRoadmapIdLess` | `src/remotecontrol_terminal.cpp` | parses the text after the final `-` |

`rcIsNonconformingIdToken` is the one that bites. Its own comment says a token
failing both regexes yields a bare `found:false` that "reads as 'the item
vanished'" — so without this widening a lookup on a synthesised id gets the
refusal the function exists to prevent.

`rcRoadmapIdLess` degrades rather than fails: an unparseable suffix falls back
to a lexicographic compare on the full id. It is widened so synthesised ids
sort by their own number rather than as text, which keeps `ANTS-S9` before
`ANTS-S10`.

### 4.5 The § 2.6 re-match companion

Not optional. A stored row holds `ANTS-S0001` while a person filing that item
by hand writes a conventional id, so ANTS-3765 § 2.6's id match fails and the
migration produces a fresh insert plus an orphan. ANTS-4343 is evidence that
path is used.

So § 2.6 is amended: an id-bearing item that finds no id match falls back to
§ 2.6.1's id-less key before being treated as new. This spec carries the
amendment; ANTS-3765 § 2.6 is edited in the same change.

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
  `synthIdsUnique` — migrate a plan of several id-less bullets, assert the
  stored ids are distinct.
  *Breaks when:* the synthesis counter is read but not advanced between
  allocations.

- **INV-4** — A synthesised id is reachable by every locating read and write.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `synthIdAddressable` — fetch a synthesised id, then `op:"annotate"` it, and
  assert neither refuses.
  *Breaks when:* a validator in § 4.4 is left requiring a digit suffix.

- **INV-5** — An item already holding a synthesised id keeps that id across a
  re-migration.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `existingIdsUnchanged` — migrate, record the ids, re-migrate the same source,
  assert every id is unchanged.
  *Breaks when:* the change is applied retroactively, against § 3 decision 1.

- **INV-6** — An id-bearing item whose id matches no stored id is matched on
  § 2.6.1's id-less key before being treated as new.
  *Test:* `tests/features/roadmap_synth_id/test_synth_id.cpp`, case
  `reMatchFallsBackToKey` — migrate an id-less bullet, rewrite the source
  bullet with a hand-written id and the same headline, re-migrate, assert the
  item count is unchanged and no orphan is reported.
  *Breaks when:* § 2.6 treats an id miss as a new item.

## 6. Failure modes

**The prefix itself ends in `-S`.** A project whose declared prefix is `FOO-S`
would produce a synthesis counter at `FOO-S-S`. That is well-formed and
distinct, so allocation stays correct; the rendered id is merely ugly. No
guard is added, because a guard would have to reject a legal declared prefix.

**A source file already contains an `-S`-shaped id.** The corpus sweep found
none, so this is a future state rather than a present one. Such an id parses
as `parsed`, and § 2.8 step 2's plan-side term ignores it because its suffix
is not an integer. It therefore raises neither counter. It is matched by id
like any other parsed id.

**The synthesis counter row is missing.** `idHighWater()` returns `nullopt`
for a project that has never synthesised, which is the ordinary first-run
state. Allocation starts at 1, exactly as ANTS-3765 § 2.8 step 4 requires for
the real prefix.

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
| § 4.1's rendered form is stable once shipped | **nothing** — no check can see a future change of mind; ANTS-3765 § 2.8 step 3 states the permanence |

## 11. Cross-doc impact

- `docs/specs/ANTS-3765-roadmap-migration-load.md` — § 2.8 gains the synthesis
  namespace and its counter; § 2.6 gains the id-less-key fallback of § 4.5.
- `docs/standards/roadmap-format.md` — § 3.5.1's id grammar admits the `-S`
  infix. This project is upstream of the global copy, so the global copy is
  corrected to match.
- `CHANGELOG.md` — a bullet stating what shipped, authored rather than copied
  from this item's headline, which states a problem.

## 12. Cold-eyes loop log

Rows live in `../reviews/ANTS-4500-synthesised-id-namespace-loop-log.md`.
