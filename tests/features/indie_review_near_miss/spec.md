# Corroboration near misses (ANTS-4817)

## Context

`indie_review_corroborate` keys on exact `(file, line)`. Two lanes that find
ONE defect and quote it a line apart therefore read as no agreement at all.

That is not an edge case. Two readers quoting one statement rarely pick the
same line: a multi-line call, a decorator or a docstring puts the quotable line
somewhere different for each. So exact matching makes the *highest-value*
agreements the least likely to be reported, and nothing in the envelope
signalled that a near miss had occurred.

Two independent projects reported it with the same mechanism and the same rate
— every real agreement in their runs was missed, one 1 of 1 and one 2 of 2 —
each on a run whose envelope was otherwise healthy: citations seen, all
resolved, and `total_findings:0`.

## Contract

The corroboration pass also reports **near misses**: groups in one file whose
lines sit within `kNearMissLines` of each other and whose distinct lane count
would have met `min_lanes` had the lanes agreed on a line.

Near misses are **advisory**:

- They are not findings, and are not counted in `total_findings`.
- They do not change what corroboration means. Corroboration is a claim about
  agreement, so promoting a group is something a caller opts into (below),
  never something a run does on its own.
- The envelope omits every near-miss field when there are none, so a run with
  no near misses is byte-identical to before.

`lineSlop` (`line_slop` at the verb) is the opt-in other half. When > 0,
citations in one file within that many lines are grouped and a group with
enough distinct lanes becomes one finding carrying the span's end (`lineTo` /
`line_to`). A group promoted this way is not *also* listed as a near miss.

**The default stays 0**, which is what lets the two coexist: at 0 the run means
exactly what it always meant, and the same groups are still visible as advisory
near misses. Both reporting projects asked for that default in as many words.

## Invariants

INV-1 and INV-5 are the new-behaviour pins and go red against the pre-fix
engine. INV-2 to INV-4 and INV-6 are boundary pins — each names something that
must *not* be grouped, or must *not* change — so they hold in both states by
construction. That is what they are for: the risk here is over-reach, not
absence.

- **INV-1** — two lanes citing one defect a line apart produce no finding and
  one near miss, naming the span and both lanes.
  *Test:* `Inv1AdjacentCitationsFromTwoLanesAreANearMiss`. **Fails against the
  pre-fix engine**, which reports no near misses at all.

- **INV-2** — a real agreement stays a finding and is not *also* a near miss.
  Double-counting would inflate the run's strongest signal.
  *Test:* `Inv2ExactAgreementIsAFindingAndNotANearMiss`.

- **INV-3** — citations far apart in one file are not a near miss. The
  tolerance is narrow on purpose: widening it enough to sweep in unrelated
  citations from a dense file is what would make the signal worthless.
  *Test:* `Inv3DistantCitationsAreNotANearMiss`.

- **INV-4** — a near miss needs `min_lanes` DISTINCT lanes, exactly as a
  finding does. One lane quoting two adjacent lines of its own is one lane's
  opinion, and reporting it would be the fuzzy matching both reporting
  projects explicitly asked not to have.
  *Test:* `Inv4OneLaneCitingAdjacentLinesIsNotANearMiss`.

- **INV-5** — an opt-in `lineSlop` promotes the group to one finding naming
  the span, and that group is then not also reported as a near miss.
  *Test:* `Inv5LineSlopPromotesToASpanFinding`. **Fails against the pre-fix
  engine**, which has no such parameter.

- **INV-6** — the default is 0, an omitted argument and an explicit 0 agree,
  and at 0 nothing is promoted while the group is still reported as advisory.
  This is the invariant that keeps every existing report meaning what it meant.
  *Test:* `Inv6DefaultSlopIsZeroAndPromotesNothing`.

## Deliberately not covered

The MCP envelope's field names. The engine is where the rule lives; the verb
serialises what the engine returns, and a scrape of the envelope would pin
spelling rather than behaviour.

Same-shape-different-file agreement — three lanes finding one defect SHAPE at
three unrelated locations. That is a different mechanism, filed separately, and
grouping by enclosing symbol rather than by line is its proposed route.
