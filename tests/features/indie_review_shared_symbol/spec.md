# Shared-symbol corroboration (ANTS-4814)

## Context

`indie_review_corroborate` keys on exact `(file, line)`, so several lanes
finding one defect **shape** at unrelated locations read as no agreement.

On a partition **by subsystem** that is the agreement that matters, because
lanes do not share files — identical `file:line` is the one form of agreement
the partition makes unlikely. A reported run resolved nearly all its citations
across many lane reports and returned a single corroborated finding, while its
strongest cross-lane signal was three lanes independently finding one defect
shape in three sibling views, plus a second shape across several more files.
A caller reading the envelope alone concluded the lanes barely agreed, when in
fact they agreed twice over.

Sibling of ANTS-4817, and a different mechanism: that one is
same-defect-same-file at adjacent lines, this one is
same-shape-different-file.

## Contract

The corroboration pass also reports **shared symbols**: the unqualified name of
an enclosing symbol cited by `min_lanes` or more distinct lanes in **two or
more different files**.

- Grouping is mechanical — the enclosing symbol from the file outline, reusing
  the machinery `workspace_search`'s `enclosing_symbol` already owns. There is
  no similarity scoring: both reporting projects explicitly asked not to have
  fuzzy matching, and that restraint is the useful part.
- **Unqualified**, because the measured case was one defect repeated across
  sibling classes whose qualified names differ by construction.
- **Two or more distinct files** is what keeps the three signals disjoint: one
  location is a finding, one file is a near miss, several files is this. One
  agreement is never reported twice.
- Advisory, like near misses. Conflating a shared symbol with a cited
  agreement would inflate what corroboration means.

## Invariants

INV-1 is the new-behaviour pin and goes red against the pre-fix verb. INV-2 to
INV-4 are boundary pins — each names something that must *not* be grouped, or
must *not* change — so they hold in both states by construction. The risk in
this change is over-reach, not absence.

- **INV-1** — three lanes citing one defect shape in three different files,
  inside the same-named method of three sibling classes, surface as one shared
  symbol; exact matching still reports no findings.
  *Test:* `Inv1SameShapeAcrossFilesGroupsBySymbol`. **Fails against the pre-fix
  verb**, which reports no shared symbols at all.

- **INV-2** — a symbol cited by only one lane is not agreement, however many
  places that lane cites it. `min_lanes` applies here exactly as to a finding.
  *Test:* `Inv2OneLaneAcrossFilesIsNotAgreement`.

- **INV-3** — two lanes agreeing inside one file stay a finding and are not
  also a shared symbol.
  *Test:* `Inv3SingleFileAgreementIsNotASharedSymbol`.

- **INV-4** — the field is absent entirely when there is nothing to report, so
  an ordinary run's envelope is unchanged.
  *Test:* `Inv4AbsentWhenNothingIsShared`.

## Where the rule lives

At the MCP layer, not in `IndieReviewEngine`. Grouping needs each citation's
enclosing symbol, which is resolved from a file outline — a dependency the
Qt6::Core-only engine does not have and should not gain. The engine publishes
its resolved citations with the lane that made each one; the verb groups them.

`cmdIndieReviewCorroborate` was split into a thin `m_main` guard plus
`corroborateWithRoot`, the same seam the `roadmap_log` ops use, so this can be
driven against a synthetic `caller_cwd` without a MainWindow.

## Deliberately not covered

The looser "shared backticked identifiers anywhere in the report text" pass the
first report floated. The second report's enclosing-symbol proposal was more
specific and reuses an existing capability instead of adding a second notion of
what a citation is about; the item itself says to take it.

Cross-language grouping, where one defect appears in files of different
languages and the function names do not match. Nothing mechanical groups those,
and a heuristic that tried would be the guesswork both reports ruled out.
