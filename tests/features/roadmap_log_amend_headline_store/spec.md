# A headline amended on a store-backed project — ANTS-4668 / ANTS-4683

**Status:** implemented (2026-08-26)

## Problem

`op:"amend_headline"` refused `unsupported_format` on a migrated
project, and no other op writes the `headline` column. So on a
store-backed project a headline was immutable for the life of the item.

The refusal's reasoning was sound about MARKDOWN: the headline is a
store column and the join key for the step-2 locate, so patching the
rendered line alone would be reverted by the next render. It never
reached a write going THROUGH the store — the path `op:"flip"` and
`op:"annotate"` already take.

The cost is not cosmetic. A headline STATES a finding, and a finding
can be refuted by later evidence. Body notes then accumulate the
correction while the headline goes on asserting the refuted claim, and
the render is what a person scans. The only workaround was to close the
bullet and refile under a new id, which loses the item's identity:
every commit, note and cross-reference aimed at the old id then points
at a bullet marked shipped that was never done.

## Contract

**The store route writes the column and re-renders.** Same seam as
every other store-backed write: `setItemField` → history row →
`last_modified` stamp → `commitAndRender`. No status move — a headline
amend cannot flip a bullet.

**No structural-prefix guard on that route, deliberately.** The markdown
path refuses `bad_args` on a `new_text` altering the `- <emoji> [ID] **`
prefix or the bold delimiters, because there it is editing a rendered
LINE. The column holds the headline TEXT alone; the marker, id
brackets, status emoji and bold delimiters are composed by the render
and are not in the value. There is no structure there to damage, so
importing the guard would refuse edits that are safe.

**One guard the column does need: it may not be emptied.** A bullet with
no headline cannot be located by headline again, and the render has
nothing to emit for it. That refuses `bad_args`.

**Uniqueness is enforced, exactly as on the body.** A phrase occurring
twice in the headline must not be clobbered on a guess.

## Invariants

- **INV-1** — On a store-backed project a unique `old_text` amends the
  headline: the reply is `ok`, the `headline` column holds the new text,
  and the rendered ROADMAP.md shows it. *Test:* `Inv1StoreAmendWritesColumn`.
- **INV-2** — `old_text` occurring twice in the headline refuses
  `headline_match_ambiguous` and writes nothing. *Test:*
  `Inv2AmbiguousRefusesAndWritesNothing`.
- **INV-3** — `old_text` absent from the headline refuses
  `headline_match_not_found`; where the phrase is in the BODY the reply
  hints at `amend_body` rather than leaving the caller to conclude the
  text is absent. *Test:* `Inv3AbsentRefusesAndHintsAtBody`.
- **INV-4** — a `new_text` that would leave the headline empty refuses
  `bad_args` and writes nothing. *Test:* `Inv4EmptyHeadlineRefused`.
- **INV-5** — `dry_run:true` reports the amend without writing the
  column or the file. *Test:* `Inv5DryRunWritesNothing`.
- **INV-6** — the op no longer refuses `unsupported_format` on a
  store-backed project. Stated separately from INV-1 because a refusal
  swapped for a DIFFERENT refusal would satisfy neither, and the
  regression this item exists for is the code, not the success.
  *Test:* `Inv6NoUnsupportedFormatOnStoreProject`.

## Out of scope

- The markdown path, which already worked and is covered by
  `tests/features/roadmap_log_amend_body/`.
- The trailer COLUMNS (`layman`, `kind`, `source`, `lanes`,
  `evidence`), which no op can rewrite after creation — the sibling
  defect, ANTS-4667.
