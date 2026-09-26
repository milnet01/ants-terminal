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
  the project, which is the shape `render_gate_unmet` HAD when both items were
  filed. That gate was scoped to the touched items on 2026-08-24 (ANTS-4628);
  the argument here is unaffected, because drift is measured per FILE and a
  drift refusal would have no equivalent scope to narrow to.

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

### ANTS-4695 — punctuation is a third population, not a restyle

`contentKey()` strips every non-alphanumeric, so two lines differing only in
how they END share a key and the second scored as a dialect restyle. That is
wrong in the one place it costs something: the Layman line. The markdown parse
drops one trailing period on the way into the store (ANTS-1154 INV-4), so a
migrated project whose Layman lines were hand-authored with periods differs
from its own render on every one of them.

The reported run: thirty items, `discarded_restyled_lines` 30,
`discarded_text_lines` **0**, `ok:true` — and after the write not one Layman
line still ended in a period. `discarded_text_lines` is the field a caller
checks before allowing a render to overwrite their file, so the number that
made the write look safe was the number hiding the change.

The true arm now also carries `discarded_repunctuated_lines`, and `check_sync`
carries `drift_repunctuated`. Both ride the true arm only, like their
siblings. `discarded_text_lines: 0` goes back to meaning *your text is
untouched*.

ANTS-4965 — a file line that differs from its render twin only in whitespace
(indentation or column alignment) is counted as `discarded_structure_lines`
(`would_discard_structure_lines` on a dry run, `drift_restructured` under
`check_sync`), and not as restyling. Each field is emitted only when non-zero.
*Test:* `Ants4965CountsWhitespaceChangesAsStructure`.

ANTS-5327 — a file line with no render twin is not lost if its words survive
as a reflow. Adjacent unmatched lines are tested as one run: if the run's
content key occurs, word-aligned, in the whole render's content key, every
line in it counts as `discarded_restyled_lines`. A run that fails is tested
line by line the same way. A run or line of fewer than three words is never
excused, so a short phrase matching by chance stays lost. *Test:*
`Ants5327CountsAReflowAsRestyledNotLost`.

ANTS-5286 — a line with no letter or digit, such as a `***` rule, is layout
and never counts as lost text. `contentKey()` also drops a leading task-list
checkbox (`- [x] `), whose `x` otherwise kept every completed github-task-list
bullet from matching its ants-v1 twin. *Tests:*
`Ants5286LineWithoutTextIsNotLostText`; the checkbox case is covered by
roadmap_convert's real-run cases, which refuse `text_lost` without it.

ANTS-4957 — the true arm also carries `discard_reason` (`would_discard_reason`
on a dry run): the worst thing at stake, one of `text_lost`, `structure`,
`punctuation` or `restyle_only`. `restyle_only` is a stale render nobody
edited, safe to overwrite. *Tests:* `Ants4957RestyleOnlyDriftSaysSo`, plus
reason assertions in the ANTS-4615, ANTS-4695 and ANTS-4965 cases.

ANTS-4844 — `op:"flip"` and `op:"annotate"` echo the bullet the render will
emit: `would_be_bullet` on a preview, `bullet` on a write. Flip EDITS an
existing bullet where append adds a new one, so it is the higher-stakes of
the two and had the weaker preview — append has echoed its would-be bullet
since ANTS-2077. `bytes` is no substitute: this path emits none, and on the
markdown path it measures something different from append's.

The bullet is rendered INSIDE the write sequence, in the window where the
mutated row exists. A preview rendering it after `commitAndRender()` returned
would show the PRE-flip state, because a dry run has rolled back by then — a
preview confidently showing the wrong thing, which is this item's complaint
reproduced rather than fixed. Per dialect, since a pass-headings project must
not be shown an ants-v1 bullet its file will never contain (ANTS-4803). The
preview and the write must agree. *Tests:*
`Ants4844FlipDryRunEchoesThePostFlipBullet`, which asserts the previewed
status is the post-flip one and fails against a pre-state render, and
`Ants4844AnnotateEchoesTheBulletCarryingTheNote`.

ANTS-5263 — that whole-bullet echo is OPT-IN, via `return:"full"`. With no
`return`, a store-served flip or annotate carries only `post_bullets`, the
compact {id, status, headline} shape, which still shows the post-flip status.
`return:"headline_only"` gives the same compact form. *Tests:*
`Ants5263DefaultEchoIsCompact` (preview and write carry no `bullet` or
`would_be_bullet`, and `post_bullets` names the item as `shipped`) and
`Ants5263HeadlineOnlySuppressesTheBulletEcho`. The two ANTS-4844 cases pass
`return:"full"`.

ANTS-4839 — a lost-text arm also carries `discard_hint`
(`would_discard_hint`): the discarded lines belong to bullets the store
holds, so they are an older publication of this same store. The claim is
exact rather than a heuristic — ANTS-4141's guard refuses the write outright
when the file holds a bullet the store never imported, and runs before the
dry-run return, so reaching the hint proves every id is known. It must NOT
claim nothing was lost: a hand-edited body belongs to a known bullet and
lands in the same count. Absent when no text was lost, so a hint on every
write is not one nobody reads. *Test:*
`Ants4839LostTextHintNamesTheStoreWithoutClaimingSafety`, which asserts both
the presence of the narrowing claim and the absence of a safety claim.

**The punctuation behaviour itself is unchanged, deliberately.** INV-4
prescribes the chop; the defect was reporting it as something it is not. The
reporter offered either fix and named this one as the one that matters.

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

- **ANTS-4593 / a preview's own candidate is not a gate failure.** A dry run
  runs `mutate` inside the transaction and rolls it back, so the proposed row
  is in the store when the gate evaluates. Its id is reported under
  `request_gate_failures`; `gate_failures` keeps only ids that really exist,
  and when the proposed row is the only offender the message says the refusal
  is about the arguments. A real write passes no candidate ids and is
  unchanged. *Test:* `Ants4593PreviewOwnIdIsNotAGateFailure`.

### ANTS-5087 — § 4's two cost claims, locked

Both cases are **structural** — a source scrape over one function body via
`ants_test::slurpFunctionBody` — and that is the honest shape here, because
neither change alters any output. A behavioural assertion that could see either
one does not exist.

- **`Ants5087PreImageRendersBeforeBeginImmediate`** — the pre-image render runs
  before `store.begin()`. § 4 says the write transaction is held across the
  validating render only, and warns that reading it as "both walks" doubles the
  lock window for nothing. The pre-image ANTS-4462 / ANTS-4465 added later sat
  inside the transaction, so the warning came true by a later edit. The
  pre-image reads the store before `mutate()` touches it, so the measurement is
  the same on either side of `begin()`; the drift cases above are what prove
  that, and this case proves only where the lock opens.
- **`Ants5087RenderReadsEveryItemInOneQuery`** — `RoadmapRender::render()` reads
  items through `RoadmapStore::readItems()` and no longer calls `readItem()`.
  § 4 named that reader as the remedy for the render's N+1 "if it lands"; it
  landed, and `roadmapstore.h` says in terms that the render "builds exactly
  this hash by hand today".

**Run RED first, against pre-fix source (2026-09-18)** — not against a
mutation. Both failed on their assertions, not on a compile: the pre-image call
was found *after* `store.begin(`, and the render's body carried `readItem(`
and no `readItems(`.

**Would break this**

- Putting any new diagnostic read inside the transaction → the first case. A
  read that does not need the write lock does not take it.
- Reverting the render to a `readItem()` per item → the second case, and § 4's
  walk cost with it.

### ANTS-5267 — the gate's prose caps the ids it names

| Case | Asserts |
|---|---|
| `Ants5267GateProseCapsTheNamedIds` | A `flip_batch` over 30 open items with no `Layman:` line refuses `render_gate_unmet`. `gate_failures` holds all 30; the `error` prose states the true total (`30 open item(s)`), says `+5 more`, and does not name the 30th id. |

Shipped 2026-09-21 (fcc4daf5) without a test; this case was added
2026-09-25. **Would break this:** joining every id into the prose again.

### ANTS-4982 — append_batch falls back to the call-level status

| Case | Asserts |
|---|---|
| `Ants4982CallLevelStatusIsTheFallback` | An `append_batch` with a call-level `status:"planned"` applies a bullet that carries no status as `planned`, and a bullet carrying `in-progress` as `in-progress`; nothing lands in `skipped[]`. |

**Would break this:** reading each bullet's status without the call-level
fallback, which refused every such bullet `bad_status`.

### ANTS-5358 — flip_batch takes a per-locator to_status

| Case | Asserts |
|---|---|
| `Ants5358PerLocatorStatusOnTheStore` | On a migrated project, a call-level `shipped` with one locator carrying `in-progress`: the plain locator's item is `shipped`, the other `in-progress`. |
| `Ants5358EveryLocatorOwnStatusNeedsNoCallLevel` | With no call-level `to_status` and a status on every locator, each item gets its own and the envelope carries no batch-wide `to_status`; one locator without a status then refuses `missing_field`. |
| `Ants5358PerLocatorStatusOnTheFile` | The same mixed batch on a project the store does not serve writes ✅ and 🚧 on the two bullets in ROADMAP.md. |

**Would break this:** applying the call-level status to every located item.

### ANTS-5344 — append_batch takes a per-bullet section

| Case | Asserts |
|---|---|
| `Ants5344PerBulletSectionOnTheStore` | On a migrated project, a bullet with no `section` lands under the call-level `work`, and a bullet carrying `bundles` lands under Bundles. |
| `Ants5344UnknownPerBulletSectionSkips` | A bullet naming an unknown section lands in `skipped[]` with `bad_section`; the other bullet still applies. |
| `Ants5344PerBulletSectionOnTheFileRefuses` | On a project the store does not serve, a bullet whose section differs from the call-level one refuses `unsupported_format` and ROADMAP.md is unchanged. |

**Would break this:** filing every bullet under the call-level section.

### ANTS-5353 — a folder-derived id prefix warns

| Case | Asserts |
|---|---|
| `Ants5353GuessedPrefixWarnsOnTheFile` | On a file-served project with no ids and no declared prefix, `append` succeeds and `warnings[]` carries `id_prefix_guessed` naming `PROJ`, the folder's letters. |
| `Ants5353GuessedPrefixWarnsOnTheStoreBatch` | The same on a migrated project with no ids, through `append_batch`. |
| `Ants5353ChosenPrefixDoesNotWarn` | On a project whose ids already carry `DEMO`, an `append` without `id_prefix` and one with it carry no `id_prefix_guessed` warning. |

**Would break this:** falling to the folder name without saying so.

### ANTS-5371 — append_batch on a store-served project with no items

| Case | Asserts |
|---|---|
| `Ants5371EmptyStoreProjectTakesABatch` | A migrated project whose roadmap has a section and no bullets takes an `append_batch`; the new item is stored `planned`. |

**Would break this:** running the file path's zero-bullet format gate when the store serves the project.

### ANTS-5283 — a dry run says whether the render would change anything

| Case | Asserts |
|---|---|
| `Ants5283DryRunSaysWhetherTheRenderWouldChange` | On a freshly migrated project, `op:"render" dry_run:true` reports `would_change:true`; after a real render, the same dry run reports `would_change:false`. |

**Would break this:** leaving the caller to infer the answer from `would_write`, or deriving it from `would_discard_external_edits`.

