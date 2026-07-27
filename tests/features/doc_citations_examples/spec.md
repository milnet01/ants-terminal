# doc_citations_examples — the `doc-examples` suppression region (ANTS-3659)

Contract: [`docs/specs/ANTS-3659.md`](../../../docs/specs/ANTS-3659.md) § 2
(the marker, the mask, what it suppresses, the antecedent reset) and § 3
(INV-49 … INV-55).

The citation grammar is `tests/features/doc_citations_scan/`; the read path is
`tests/features/doc_citations/`. This directory covers only the region mask
layered over both. Same bundle as its siblings (`test_core`), and the
`check`-layer fixtures come from `../doc_citations/fixture.h` — a divergent copy
of its canonical-root logic would make these tests pass or fail for reasons that
have nothing to do with regions.

## Contract

- **INV-49** — nothing whose `docLine` is masked reaches either array.
  *Test:* `Inv49RegionSuppressesBothArraysAndCounts`.
- **INV-50** — the mask's boundary cases: fenced marker is not a marker, a
  marker in a multi-line inline span is, and a citation is tested by its own
  `docLine` rather than its span's opening line.
  *Test:* `Inv50FenceSpanAndDocLineBoundaries`.
- **INV-51** — unterminated / degenerate markers: first-opener reporting, stray
  `end`, nested `begin`, and the unclosed fence that swallows an `end`.
  *Test:* `Inv51UnterminatedAndDegenerateMarkers`.
- **INV-52** — `examplesSuppressed` counts both arrays, whole-doc.
  *Test:* folded into `Inv49RegionSuppressesBothArraysAndCounts`.
- **INV-53** — a masked line resets the sticky antecedent; a stray `end` does
  not. *Test:* `Inv53MaskedLineResetsAntecedent`.
- **INV-54** — the three mechanical spelling rules, from both sides.
  *Test:* `Inv54MarkerSpellingBothSides`.
- **INV-55** — the JSON keys' omit-when-false, and tally invisibility.
  *Test:* `Inv55JsonKeysAndTallies`.

## Why so many fixtures assert a NEGATIVE

Eleven of these rows assert that something is **not** suppressed. Against
feature-absent code every one of them passes trivially, because nothing
suppresses anything — a negative assertion cannot tell "correctly declined to
mask" from "the mask does nothing yet". They are the rows that guard against
*over*-suppression, which is the failure mode that silently hides real rot, so
they are the rows most worth having and the least self-evidencing.

They are therefore re-proven by mutation, not by the RED run. See below.

## Must fail first

Verified RED before the mask was wired into `DocCitations::scan` (2026-07-27),
with `MarkdownScan::exampleMask` present but its result unused — which is
`docs/specs/ANTS-3659.md` § 6's "feature-absent" state exactly: the two
`ScanResult` members sit at their sentinels and nothing consumes the mask.

Every row asserting **suppression** failed. Every row asserting
**non-suppression** passed, as predicted above.

## Mutation proof

Each wrong implementation was compiled in **alone**, built, run, and reverted
(2026-07-27). Recorded here rather than kept behind a build flag, because a
permanent mutation switch is a second implementation to maintain.

This table is what the runs actually produced, not what was predicted — three
of the predictions were wrong, and each wrong one taught something.

| Mutation compiled in | Rows it turned red |
|---|---|
| `exampleMask` ignores the `fence` argument | all three INV-50(a) rows, plus INV-51's swallowed-fence row |
| `exampleMask` consumes `codeSpans` and skips spans | INV-50(b), (c) and (d) — seven rows |
| a nested `begin` **re-opens** the region | INV-51 "keeps the FIRST opener" — **that row alone** |
| a nested `begin` increments a **depth counter** | INV-51 "no depth counter" + "closed by one end" |
| the marker regex drops its `^ {0,3}` indent allowance | both INV-54 accept rows |
| the marker regex loses its `$` anchor | INV-54 trailing-prose reject row — alone |
| the marker regex is case-insensitive **and so is the `begin`/`end` compare** | INV-54 uppercase reject row — alone |
| `record` masks `citations[]` only | INV-49's `bad_locus` row + INV-52's count |
| the guard is keyed one line off (`docLine - 2`) | six rows across four tests |

### The three predictions that were wrong

- **"the marker regex is case-insensitive" alone turns NOTHING red.** The
  lowercase rule is enforced *twice*: by the regex, and again by
  `m.capturedView(1) == QLatin1String("begin")`, which is a case-sensitive
  comparison. An uppercase marker matches a loosened regex and then satisfies
  neither `begin` nor `end`, so it falls through inert. The row is falsifiable
  only by an implementation case-insensitive *throughout*, which is the row in
  the table. Worth knowing before anyone touches that regex: the capture
  comparison is the second line of defence, and loosening one alone is silent.
- **"suppression keyed on the span's opening line" is not a writable
  implementation here.** Pass 1 harvests path-bearing tokens off the raw line
  and pass 2 accepts only single-line spans, so nothing in `scan` ever reaches
  for a span's opening line. It was replaced with an off-by-one on the guard
  index, which tests the same sensitivity and is a real defect class.
- **"suppressed tokens still counted in `unparsed_total`" is likewise
  unwritable.** A suppressed token never reaches `check`, so no downstream code
  *could* count it. INV-55's `unparsed_total:0` row is still falsifiable — by
  any mutation that breaks the mask — but it has no bespoke mutation of its own.

### What the proof bought

Two rows exist only because a review pass argued they were needed, and the
mutations confirm both arguments were right:

- INV-51's **unclosed doubled `begin`** is the only row a *re-opening*
  implementation fails; it is byte-identical to the correct one on every other
  fixture. Without it, a re-opening `exampleMask` ships green.
- INV-49's in-region **`bad_locus`** token is the only row that catches a guard
  masking `citations[]` alone. The original fixture used `6.2:1`, which is a
  *citation* at the scan layer, and would have stayed green under it.
