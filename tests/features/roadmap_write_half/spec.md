# roadmap_write_half — feature contract

Test contract for **ANTS-3809** (`docs/specs/ANTS-3809-roadmap-write-half.md`).
On a **migrated** project every `roadmap_log` op mutates the store and re-renders;
no op keeps a markdown writer of its own.

Each case builds its store by **migrating a small markdown fixture**, so the
starting state is one the migration can actually produce. A hand-built store can
hold rows the loader never writes, and an invariant asserted against one is
asserted against a state the product cannot reach.

`XDG_DATA_HOME` is redirected per case (`ants_test::XdgGuard`) so
`RoadmapStore::defaultPath()` — the path `RemoteControl` opens for itself —
lands in the case's sandbox. **`RoadmapStore` is never default-constructed:**
`defaultPath()` resolves the developer's real store, and `Access` is the
**third** constructor parameter, after `historyCapBytes`.

## Cases

| Case | Invariant | Asserts |
|---|---|---|
| `Inv1RenderFailureRollsBack` | INV-1 | A project whose render gate fails refuses `render_gate_unmet` **and leaves the store unchanged** — the flip did not persist behind a failed render. |
| `Inv2RenderIsTheOnlyWriter` | INV-2 | `append` on a migrated project writes the item to the **store** and re-renders: the envelope carries `files_written` / `items_rendered` and carries **no** `line` / `bytes_written`, and the new id is in both the store and the file. |
| `Inv3Allocation` | INV-3 | Allocation floors to the committed corpus, not to `.roadmap-counter` (which the store path never reads or writes): the fixture's highest id is `DEMO-0007`, so two appends give `DEMO-0008` then `DEMO-0009`, an `id_hint` at or below the high-water is `id_taken`, and `.roadmap-counter` is still absent afterwards. |
| `Inv4BodyDerivesColumns` | INV-4 | An `annotate` whose note adds a `Lanes:` line sets the **lanes column** — the mechanism that makes the render's `Layman:` gate remediable. A key neither body yields is left untouched. |
| `Inv5BodyShadowed` | INV-5 | `append` with `kind:"fix"` and a body carrying `Kind: refactor.` refuses `body_shadowed`, names the remedy for an **anchored** match, and writes no item. |
| `Inv6LineRangeRefused` | INV-6 | `flip_batch`'s `line_range` locator is refused **per locator** with `locator_unsupported` into `skipped[]`, while an `id` locator in the same batch still applies. |
| `Inv7DryRunCommitsNothing` | INV-7 | `append` with `dry_run:true` returns `ok` + `dry_run` and commits nothing: no new item in the store, and `ROADMAP.md` is byte-identical. |
| `Inv8BundleRow` | INV-8 | `bundle_row` is a read-modify-write of one `kind='table'` element's canonical-JSON payload — a **store** assertion: `"rows"` gains the row, `"header"` is unchanged, and no second table element is inserted. |

## Must-fail-first — run, not asserted

Six of the eight have no store write path at all before ANTS-3809, so their
proof is run against the implementation with **the rule under test removed**.
Each mutation below was applied, built, and the case observed RED, then reverted
(2026-08-05):

| Case | Mutation | Observed |
|---|---|---|
| INV-1 | the GateUnmet path commits instead of rolling back | the flip persisted behind the refusal |
| INV-2 | `append`'s store branch disabled — the pre-fix markdown splice | envelope carried `line` + `bytes_written`; nothing in the store |
| INV-3 | the corpus floor dropped from `rlStoreIdHighWater()` | allocated `DEMO-0001`, and the `id_hint` was accepted |
| INV-4 | `rlDeriveTrailerColumns()` returns immediately | the note's `Lanes:` reached the body and not the column |
| INV-5 | the shadow check gated on `anchored == false` | the stale `Kind:` trailer line no longer refused |
| INV-6 | the `line_range` refusal removed | the range matched every bullet |
| INV-7 | `dry_run` forced false into `commitAndRender()` | the preview committed the item and rewrote the file |
| INV-8 | `setElementPayload()` replaced by `addElement()` | a second `kind='table'` element |

## Would break this

- Falling back to the markdown splice when the store is present → INV-2.
- Rendering before committing the store → INV-1's rollback window inverts.
- Allocating from `idHighWater()` alone → INV-3: a fresh clone reissues a live id.
- Clearing a trailer column the new body does not yield → INV-4's two-op sequence.
- Gating the shadow check on `anchored == false` → INV-5 misses the commonest shape.
- Resolving `line_range` against the store's zeroed `firstLine` → INV-6 flips everything.
