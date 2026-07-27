# doc_citations_anchor — the anchor-symbol drift check (ANTS-3654)

Contract: [`docs/specs/ANTS-3654.md`](../../../docs/specs/ANTS-3654.md) § 2 (anchor,
needle, match) and § 3 (INV-13, INV-14).

The engine's read path is `tests/features/doc_citations/`; the citation grammar is
`tests/features/doc_citations_scan/`. This directory covers only the advisory
verdict layered on a resolved `ok` citation. Same bundle as the parent
(`test_core`), and the fixture is shared via `../doc_citations/fixture.h` — a
divergent copy of its canonical-root logic would make these tests pass or fail for
reasons that have nothing to do with anchors.

## Contract

- **INV-13** — an anchor is the immediately preceding identifier code span within
  `maxAnchorGap` on the citation span's opening line; the needle is the text after
  its last `::`; the match is case-sensitive and whole-identifier over the full
  resolved range. `anchor_symbol` carries the span verbatim, never the needle. Both
  fields are absent when there is no anchor or the status emits no text.
  *Tests:* `DocCitationsAnchor.Inv13AnchorTable` (13 rows),
  `DocCitationsAnchor.Inv13GapBoundaryIsInclusive`,
  `DocCitationsAnchor.Inv13FullRangeVerdictLeavesTextClipped`.
- **INV-14** — `only:"stale"` keeps non-`ok` entries and `ok` entries with
  `anchor_found:false`, never filters `unparsed[]`, and leaves the tallies
  whole-doc. *Test:* `DocCitationsAnchor.Inv14StaleKeepsAnchorMissing`.

## Why the gap boundary is a pair, not a row

`gap == maxAnchorGap` and `gap == maxAnchorGap + 1` are asserted together because
either one alone passes under both `gap < maxAnchorGap` and `gap <= maxAnchorGap`.
A single row would leave the boundary unpinned while looking like it covered it.

## Why one row asserts `text` as well as the verdict

`Inv13FullRangeVerdictLeavesTextClipped` is the regression guard for the widened
read. The verdict must come from the full cited range while the response stays
clipped to `max_range_lines`; an implementation that widens the read by widening
what it appends to `text` breaks the parent's contract on every anchored citation,
and `range_truncated` will not catch it — that flag is derived from the citation's
own line numbers, not from what was read.

## Must fail first — and which rows genuinely can

Verified RED against the pre-implementation engine (2026-07-27), where
`anchor_symbol` / `anchor_found` are never emitted and `counts.anchor_missing` is
hardcoded `0`. All four tests fail. But the coverage is not uniform, and saying
"13 rows, all red" would overstate it:

- **8 of INV-13's 13 rows fail for their stated reason** — every row expecting an
  anchor reports "expected both anchor fields".
- **5 rows expect *no* anchor and therefore pass trivially**, because the fields
  are absent for every citation today. A negative assertion cannot tell "correctly
  found no anchor" from "the feature does not exist". They earn their keep only
  once the check is implemented, where they guard against over-anchoring — the
  `` ` foo ` `` strip rule, the non-identifier span between anchor and citation,
  the bare citation outside any code span, `Foo::`, and `missing_file`.
- INV-14 fails on exactly the three values the check owns (`returned`,
  `anchor_missing`, `unchecked`); the rest of its fixture — `count:7`,
  `counts.ok:6`, `unparsed_total:1`, `len(unparsed[])` — already passes, which
  confirms the fixture's arithmetic against the real engine rather than against
  the spec's own prose.

So the negative rows needed a second proof, and it was taken once the check
existed (2026-07-27): five deliberate defects were compiled into
`anchorFor`/`check`, one per negative row, and **each row went red under its own
mutation and no other**. A row that stays green under a deliberately over-eager
matcher is not testing anything.

| Mutation compiled in | Row it turned red |
|---|---|
| Scan back to the *nearest* identifier span instead of the immediately preceding one | non-identifier span between anchor and citation |
| Apply CommonMark's one-space strip to span content | `` ` foo ` `` |
| Drop the empty-needle discard | `Foo::` |
| Treat a citation outside every span as its own delimiter-less span | citation not inside a code span |
| Emit the verdict for a status that carries no text | `missing_file` with a good anchor |

The mutations were reverted and the suite re-run green; they are recorded here
rather than kept behind a build flag, because a permanent mutation switch is a
second implementation to maintain.
