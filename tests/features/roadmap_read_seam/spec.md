# roadmap_read_seam — feature contract

**Covers:** ANTS-3793 (`docs/specs/ANTS-3793-roadmap-consumer-cutover.md`),
INV-1 / INV-2 / INV-3.
**Bundle:** `test_core` — the only bundle linking both `ants_core_lib` and
`ants_roadmapstore_lib`.

## What this pins

The read seam: `RoadmapSource`'s four functions, which let a consumer take its
`BulletRecord`s from the roadmap store instead of from `ROADMAP.md` without
changing what a record contains.

| Case | Invariant | Asserts |
|---|---|---|
| `Inv1DispatchMarker` | INV-1 | five dispatch outcomes in one process |
| `Inv2BackendsAgree` | INV-2 | record-for-record equality, store vs the rendered file |
| `Inv2Membership` | INV-2's membership half | `dropped` excluded, `internal` kept, unfiled kept and last |
| `Inv3Ceiling` | INV-3's refusal | 3,501 items refuse, 3,500 do not |
| `Inv3Latency` | INV-3's budget | p95 < 50 ms on a corpus-sized project |

## Cases

### `Inv1DispatchMarker`

Five outcomes, and the two that protect every *unmigrated* project belong to
`storeFor()` rather than to `bulletsFor()` — an absent store never reaches the
seam at all, which is why that decision is a free function this case can drive
directly.

1. **No store file** → `storeFor()` returns `nullptr`, `why == None`. Serve
   markdown. This is the case that protects every machine that has never
   migrated anything.
2. **Queried before and after loading** → the same project, same process:
   `nullopt` / `why == None` before the migration commits, engaged after. The
   marker is resolved per CALL; a cached one would serve markdown for the rest
   of a session to a project migrated in between.
3. **A loaded pass-headings project** → markdown, `why == None`. A store row
   existing does not imply this path can serve the project: the migration reads
   all three dialects and the store backend serves `ants-v1` only. Refusing here
   would break every migrated GFM or pass-headings project.
4. **A store file that will not open** → refuse, `why == StoreFailed`. A
   corrupted store that quietly falls back is a store nobody notices is corrupt.
5. **A loaded project whose roadmap text is empty** → refuse,
   `why == SourceUnrecognised`, *not* markdown. `detectRoadmapFormat()` answers
   `"ants-v1"` for input it does not recognise, so the returned dialect cannot
   separate this from case 3 — `sawSignal` is what does.

Case 5's error is deliberately not `StoreFailed`: the store is fine and the
*file* is not, and the two send the user to different places.

Canonicalisation is asserted alongside: a project registered under its real path
resolves from a non-normalised spelling of the same path (`…/proj/.`), because
`readProjectByRoot()` is keyed on the canonical root and a raw-path lookup would
miss and report "not migrated" — the silent fallback INV-1 forbids, arriving
through the one door the invariant does not watch.

### `Inv2BackendsAgree`

Migrate a fixture's markdown, render that store back to markdown, parse **the
live rendered file**, and compare record-for-record against
`bulletsFromStore(…, includeArchive = false, …)`.

The comparison is against the *rendered* text and not the source fixture, and
that is the invariant's substance rather than a testing convenience: a migrated
project's `ROADMAP.md` **is** the render's output, so the rendered text is what
the markdown backend will actually be handed.

Equality over the 20 compared fields; `firstLine` and `lastLine` are asserted to
be **0** on the store path rather than compared — a store has no lines to number
and no walk can invent them. That is what "field-for-field over 22 members with
two declared differences" means operationally.

The fixture carries the shapes the field table says a naive implementation gets
wrong: an item whose `id` column is empty (renders no bracket, so `id`,
`idToken` and `boldId` all come off the head line instead), a duplicated heading
title (the parser's slugger is stateful, so the second gets `-2`), preamble
bullets under the level-0 synthetic root (which emits no heading, so their
`sectionHeading` and `sectionSlug` are empty), and an over-long headline (which
truncates at 120 characters and then appends `…`).

### `Inv2Membership`

INV-2 scopes its equality to the renderable, filed subset, which excludes
`internal` and unfiled items *by definition* — so the three membership rules
that decide what `bulletsFromStore()` returns would otherwise ship with no case
at all.

Populates a store **directly**, not via migration, because migration cannot
produce any of the three: `visibility` defaults to `public` and no migration
path writes it, `statusFromMarker()` cannot produce `dropped`, and every
migrated item is filed. One `internal`, one `dropped` and one unfiled item
beside two ordinary ones, at `includeArchive == false`:

- the `dropped` item is **absent** — `emojiFor()` returns an empty string for it
  by design, so its head line carries no status marker and the parser declines
  it exactly as a document walk would skip it;
- the `internal` item is **present** — `BulletRecord` has no visibility field,
  so the record is well-formed, and filtering here would give `roadmap_query` a
  visibility concept it has never had;
- the unfiled item is **present**, with an empty `sectionSlug`, sorted after
  every filed item. Running at `includeArchive == false` is also what pins that
  unfiled items survive the live-only scope: they have no section, therefore no
  `sourcePath` for the flag to test.

### `Inv3Ceiling`

Two generated projects sized **from the ceiling**: 3,501 items must refuse with
`TooLarge`, and 3,500 must not — which is what pins `>` rather than `>=`. The
refusal fires on `listItems().size()`, before any `readItem()` runs, so a
project over the ceiling never materialises the bodies the ceiling exists to
bound.

The count is a deliberately over-inclusive proxy for a 16 MiB record budget: it
counts every item row, including the `internal` and `dropped` ones the reader
does not return. No case measures resident bytes — 16 MiB is a sizing input, not
something this invariant asserts.

### `Inv3Latency`

Warm p95 over repeated whole-project reads of a corpus-sized project, against
the 50 ms budget.

**A timing assertion on a loaded host is a flake generator**, so this case
carries the `perf` label and the ceiling assertions do not — which is what § 6
means by "one ctest registration cannot be half-labelled".

The label is applied in `tests/slow_test_timeouts.cmake` rather than in
`CMakeLists.txt`, because bundle cases are discovered with
`gtest_discover_tests(DISCOVERY_MODE PRE_TEST)` and no test exists at configure
time for `set_tests_properties` to name. CTest reads `TEST_INCLUDE_FILES` after
discovery, which is the seam ANTS-3658 already built for its own per-case
`TIMEOUT`. Setting `LABELS` there *replaces* the bundle's `features;fast`, so
the case leaves the default presets and the pre-push gate — both filter
`-LE 'e2e|perf'` — and joins `ctest --preset=perf`.

Naming a test that does not exist in that file is **silently ignored**, so a
rename of `Inv3Latency` would quietly put a timing assertion back into the
pre-push gate. `Ants3793LatencyCaseIsPerfLabelled` is the guard for that.

## Mutations these fail against

Per this project's convention every case is verified RED against its *Breaks
when* mutation before the implementation is restored.

| Case | Mutation |
|---|---|
| `Inv1DispatchMarker` | resolve the marker once and cache it; fall back silently when the store will not open; refuse a recognisable foreign dialect instead of serving it markdown; look the project up by its raw path instead of its canonical one |
| `Inv2BackendsAgree` | order by `item_pk` instead of § 2.1.3's walk; return `item.body` raw; fill `id`/`idToken` from the `id` column; copy `status` verbatim instead of the emoji; drop `headline`'s 120-character ellipsis; copy a `level == 0` root's title into `sectionHeading`; slug sections statelessly |
| `Inv2Membership` | return `dropped` items; filter `internal` ones; drop unfiled items, or emit them before the filed ones |
| `Inv3Ceiling` | test `>=` instead of `>`; test the ceiling after the item bodies are already resident |
