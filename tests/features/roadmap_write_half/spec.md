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
| `Ants4462ReportsDiscardedExternalEdits` | ANTS-4462 / ANTS-4465 | The publish reports what it overwrote that the store never held. Four arms in one case, because the sequence is the contract: the FIRST write after a migration reports drift (the file still carries the author's table separator where the store keeps a canonical one — real bytes, really overwritten); the second reports none, because the first republished the file; a hand-edited preamble line makes it report again, with a count and **without** becoming a refusal; and the write after that is quiet, so the flag cannot go stale. |
| `Ants4462DryRunUsesTheFutureTense` | ANTS-4462 / ANTS-4463 | A preview carries `would_discard_external_edits` / `would_discard_edit_lines` and **neither** past-tense name — ANTS-4463's rule reaching two new fields — and leaves the hand-edit on disk. |
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

The two ANTS-4462 cases were run RED against **pre-fix source** rather than
against a mutation (2026-08-19): the fields did not exist, so both failed on
assertions — not on a compile — with `would_discard_external_edits` absent and
its count reading 0. Asserting a field by name is exactly the shape that passes
for the wrong reason when the seam is below the code under test, so the red run
was performed and not inferred.

## Would break this

- Falling back to the markdown splice when the store is present → INV-2.
- Rendering before committing the store → INV-1's rollback window inverts.
- Allocating from `idHighWater()` alone → INV-3: a fresh clone reissues a live id.
- Clearing a trailer column the new body does not yield → INV-4's two-op sequence.
- Gating the shadow check on `anchored == false` → INV-5 misses the commonest shape.
- Resolving `line_range` against the store's zeroed `firstLine` → INV-6 flips everything.
- Measuring the drift against the **post**-mutation render → every healthy write
  reports the change it was called to make, and the field is switched off.
- Comparing raw mtimes instead → the sequence writes the store and *then* the
  file, so the file is always the newer of the two and every project reads stale.
- Turning the report into a refusal → one hand-edit anywhere bricks every op on
  the project, which is the `render_gate_unmet` shape both items were filed against.

### ANTS-4614 — `op:"render"` publishes the store on demand

`roadmap_migrate` reports `markdown_rewritten:false` honestly (ANTS-4482 shipped
the saying-so half) and **nothing owned the doing half**: the canonical
re-render landed only on the next semantic write.

On the reporting project that was not cosmetic. The file carried two id dialects
the store would normalise — 24 bullets as `- OK **LOTTO-NNNN** Headline.` and 9
as `- TODO [LOTTO-NNNN] **Headline.**` — so a real, wanted normalisation sat
undelivered with no way to publish it. Two costs: the only route was to **invent
a semantic write purely as a render trigger**, polluting the roadmap with a
bullet nobody wanted; and the migration was **unverifiable from the repo side**,
because a clean `git status` after migrating is indistinguishable from the
migration never having run.

The op is the shared write sequence with a `mutate` that does nothing. That is
the design, not a shortcut: every gate the eight semantic ops run — the Layman
gate (INV-5), ANTS-4141's divergence guard — lives in `commitAndRender`, so it
runs here too, and this op cannot become a way around them.

- **`Ants4614RenderPublishesWithoutASemanticWrite`** — the normalisation lands
  (the file really changes), the envelope names what it wrote, and the store
  still holds exactly the fixture's two items. That last assertion is the point:
  the workaround it replaces added an item nobody wanted.
- **`Ants4614SecondRenderIsQuiet`** — idempotent, and the second run reports no
  drift. This is what makes the op safe to reach for when you are simply unsure
  whether the file is current.
- **`Ants4614DryRunPreviewsAndWritesNothing`** — ANTS-4463's tense rule reaches
  this op like every other, so a preview carries `would_write` and neither
  `files_written` nor `bytes_written`, and the file is left byte-identical.

It refuses `project_not_registered` on a markdown-backed project rather than
pretending: there the file already **is** the source of truth, so there is
nothing to publish from.

### ANTS-4615 — the drift report is split, and names the lost text

`discarded_edit_lines` mixed two populations. One measured report of 84 was 24
bullets restyled into the canonical id form plus **one** sentence that no longer
existed anywhere; a single number trains callers to wave the flag through.

The true arm now also carries `discarded_restyled_lines` (text survives, styling
differs), `discarded_text_lines` (**the one to act on**), and `discarded_text[]`
naming the lost lines, capped at 20 with `discarded_text_truncated`.

**Nothing is suppressed, and that distinction is the design.**
`Ants4462ReportsDiscardedExternalEdits` says in terms that deciding which
differences are cosmetic is a judgement this check does not have and should not
invent — so the total keeps counting every drifted line in both directions, and
that case passes unchanged. ANTS-4462 says do not *suppress*; ANTS-4615 says do
not *conflate*.

Two consequences worth stating. An unclassifiable line counts as **text**:
over-reporting loss costs a look, under-reporting hides the thing being
reported. And the two sub-counters classify the **file's** lines only, so they
deliberately do not sum to the total — which also counts lines the render
restores that the file had deleted. A reverted deletion is not a loss, and
inventing an arithmetic relationship would be a third wrong number.
