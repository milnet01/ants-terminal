# Feature: `roadmap_log` warns when a review fix is filed as a plain `fix`

Test contract for ANTS-4989.

## Problem

`roadmap-format.md` v1.2 § 3.5.3 narrows `review-fix` to a fix arising from a
review. A rule with no observable is one a conformer cannot tell they
breached — which the `standard` genre calls a defect in the rule, not in the
conformer.

The gap was found by the rule 14 gate on that very change: the § 3.5.3 text
asserted this advisory already existed. It did not. All three cold lanes
caught it, and the standard was corrected to say the rule was unchecked and
to cite this item.

## Why an advisory and not a refusal

`Kind:` is a judgement. A refusal would reject correctly-filed work over a
labelling convention, and the reviewer of an item is not always the filer.
ANTS-4527's `evidence_not_path_shaped` is the precedent: an advisory on a
successful write.

## Why it is worth firing at all

Measured over the machine-global store before building: items whose `kind` is
`fix` and whose `Source:` names a review origin outnumber the review-derived
items correctly carrying `review-fix` or `audit-fix` by roughly three to one,
while firing on well under a tenth of all appends. So the rule is breached far
more often than followed, and the check is not a flood.

## Invariants under test

- **INV-1** — `op:"append"` with `kind: "fix"` and a review-shaped `Source:`
  succeeds AND carries a `kind_ignores_review_provenance` warning.
- **INV-2** — it is an advisory, never a refusal: `ok` stays true and the
  bullet is written.
- **INV-3** — no warning when the kind is already `review-fix` or
  `audit-fix`, nor when the source names no review origin. A check that fires
  on correctly-filed work trains its reader to ignore it.
- **INV-4** — every live spelling of one origin triggers it. The standard
  keeps `indie-review-`, `code-quality-review-` and `review-code-` live for
  the same origin, so a check recognising only the current name misses most
  of the corpus.
- **INV-5** — the suggestion matches the origin: an `audit-` or `check-code`
  source suggests `audit-fix`, a review source suggests `review-fix`.
- **INV-6** — `op:"append_batch"` warns ONCE for the batch, naming the
  bullets. A batch filing one review's findings is where every bullet shares
  the pairing, and N identical advisories would bury the reply.

## Test shape

Drives `RemoteControl::cmdRoadmapLog` against a seeded temp roadmap, the same
harness the other roadmap_log feature tests use. Behavioural throughout.

Label: `features;fast`.
