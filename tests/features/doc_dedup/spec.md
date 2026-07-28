# doc_dedup — engine conformance

Contract for `tests/features/doc_dedup/test_doc_dedup.cpp`. Owning spec:
[`docs/specs/ANTS-3660.md`](../../../docs/specs/ANTS-3660.md).

`DocDedup::Accumulator` is pure — document text in, pairs out — so every row here
drives it directly, with no MainWindow and no filesystem. The one exception is
`DISABLED_CorpusCalibration`, which walks the real `docs/` tree because that is
the whole point of it.

**Fixtures are built, not typed.** Every threshold row depends on an exact
shingle count, and a hand-written 100-word paragraph is one typo away from
asserting a different number than its comment claims. `tokens("aw", 30)` yields
30 distinct words, so a 30-word passage has exactly 28 3-gram shingles and every
Jaccard below is arithmetic a reader can check.

## What each row locks

| Row | Invariant | Claim |
|---|---|---|
| `Inv1ThresholdFromBothSides` | INV-1 | A pair at exactly `minSimilarity` reports; the same pair one hundredth above it does not. |
| `Inv2FenceAwareness` | INV-2 | Two identical fenced samples never pair, and a fence line breaks a paragraph rather than joining it. |
| `Inv3ExclusionsAndTheDeletedOne` | INV-3 | `minWords`, pointer lines and path globs all exclude — and a repeated spec **header block** still reports. |
| `Inv4PairsCanonicalAndUnique` | INV-4 | Three mutually similar passages give three pairs, `a` before `b` in (file, line) order, no self-pair. |
| `Inv6PassageCapCountsAll` | INV-6 | `maxPassages` caps what is compared; the walk still counts everything. |
| `Inv7StopShinglePrune` | INV-7 | The prune drops candidates and never changes a score. |
| `Inv9ClustersAreConnectedComponents` | INV-9 | A–B and B–C with no A–C is **one** cluster of three. |
| `DISABLED_CorpusCalibration` | — | § 2.3's acceptance criterion, measured against `docs/`. Not a contract; re-runnable. |

INV-5 and INV-8 are the verb lane's (`tests/features/doc_dedup_verb/`): both are
half source-scrape, which is a claim no behavioural row can hold.

## The arithmetic each threshold row rests on

Stated here because a reader checking the fixture should not have to re-derive
it, and because two of these numbers are the only reason the row is writable.

| Row | Construction | Jaccard |
|---|---|---|
| INV-1 pair A–B | 30 distinct tokens vs the same first 18 then 12 new | 16/(28+28−16) = 16/40 = **0.400 exactly** |
| INV-1 pair A–C | the same first 17 then 13 new | 15/41 = 0.366 |
| INV-7 genuine pair | 24 words each, sharing the stop-shingle plus an 18-shingle tail | 19/(22+22−19) = 19/25 = **0.760** — and **0.720** if the pruned shingle is dropped from the score |
| INV-9 A–B, B–C | 100-token base; A carries its first 60, C its last 60 | 58/138 = **0.420** |
| INV-9 A–C | the 20 tokens A and C both inherit | 18/178 = **0.101**, below threshold |

0.400 is exact in IEEE double — `16.0/40.0` and the literal `0.40` both round to
the same value — so `EXPECT_DOUBLE_EQ` is safe and the `>` / `>=` distinction is
genuinely decidable.

## Verified RED before the implementation landed

Four arms assert an **absence** of pairs and would pass vacuously against an
engine that pairs nothing (INV-2's shared-sample row, INV-3's three exclusions).
Each was re-proven by mutating the shipped engine and recording what the mutation
actually turned red. A compile failure proves nothing here.

Ten mutations, applied one at a time to `src/docdedup.cpp`, each rebuilt and
re-run. **10/10 turned red, and each turned red the row it was aimed at** — no
mutation survived, and none turned red only some unrelated row.

| # | Mutation | Result |
|---|---|---|
| M1 | Replace `MarkdownScan::fenceMask` with an all-false mask | RED — `Inv2FenceAwareness`: the shared sample pairs, and `passagesTotal` is 4/1 where 2/2 is expected. |
| M2 | Drop the pointer-line exclusion | RED — `Inv3ExclusionsAndTheDeletedOne`: the repeated long link pairs. |
| M3 | Drop the generated-artifact path globs | RED — `Inv3`: the two `superpowers/` docs pair. |
| M4 | **Reinstate** a structural-boilerplate filter (skip a paragraph opening `**Status:**`) | RED — `Inv3`'s arm (d): the shared spec header block stops reporting. This is the regression guard for § 2.4's deletion. |
| M5 | Let the prune change the SCORE — count only non-stop shingles in the intersection, as ANTS-3666's reference did | RED — `Inv7StopShinglePrune`: similarity is 0.720 where 0.760 is asserted. |
| M6 | Threshold as `>` instead of `>=` | RED — `Inv1ThresholdFromBothSides`: the exactly-0.400 pair vanishes. |
| M7 | Cluster only directly-scored pairs (drop the transitive union) | RED — `Inv9ClustersAreConnectedComponents` (3 clusters where 2 is expected) **and** `DocDedupVerb.PairAndClusterShapeReachTheWire`. |
| M8 | Emit pairs in index order, uncanonicalised | RED — `Inv4PairsCanonicalAndUnique`: `c.md` precedes `a.md`. |
| M9 | Stop the walk at `maxPassages` instead of counting past it | RED — `Inv6PassageCapCountsAll`: `passagesTotal` is 10 where 30 is expected. |
| M10 | Drop the `minWords` floor | RED — `Inv3`'s arm (a): the repeated 8-word note pairs. |

M7 turning red in **both** lanes is worth keeping: the verb lane's shape row was
written to assert the envelope, and it independently caught a clustering defect.
A row that only ever fails alongside another row is usually redundant; this one
is not.

**No mutation survived, which is itself a claim to check rather than celebrate.**
Two fixtures were sharpened *before* the run on the strength of exactly this
risk, and both would otherwise have let a mutation through:

1. **INV-3 arm (d)** originally used a short `**Status:**` line followed by a
   separate 40-word paragraph. The header line was 7 words — below `minWords` —
   so M4's filter would have had nothing to delete and would have survived. The
   fixture now makes the header block itself clear `minWords`, and asserts that
   it does (`ASSERT_GE(normalise(header).split(' ').size(), 15)`).
2. **INV-4** originally added documents alphabetically, which makes index order
   and (file, line) order identical — M8 would have survived. The documents are
   now added `c.md`, `a.md`, `b.md`.

## A real defect this lane caught

`Inv3`'s glob arm failed on first run: `QRegularExpression::wildcardToRegular`
`Expression` gives `*` **glob** semantics, where it does not cross a path
separator, so `*superpowers/*` never matched `docs/superpowers/plans/x.md` and
the exclusion silently did nothing. Qt 6.6's `NonPathWildcardConversion` says
exactly this, but the project's floor is Qt 6.2, so `isExcludedPath` writes the
two-line translation out instead. Without arm (c)'s positive control — the same
two passages under ordinary paths, which *must* pair — the row would have read
as passing for the wrong reason.

## Not covered here

- **Report-only and the refusal minimums** — the verb lane owns INV-5 and INV-8.
- **Paraphrase.** Two passages stating one idea in different words score ~0.17
  and no shingle measure reaches them (owning spec § 5). Nothing here asserts
  otherwise, and a clean run is not proof of no duplication.
- **The `doc-examples` mask.** The owning spec § 2.4 settles that regions are
  ignored, on measurement; there is no option to assert either way.
