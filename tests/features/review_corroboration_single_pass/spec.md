# review_corroboration_single_pass — one corroboration walk per round (ANTS-5125)

`IndieReviewEngine::corroboratedFindings` (and `ColdEyesEngine::crossDocDiffFromReports`,
which delegates straight to it) walks the whole project tree to build a
basename index before it can resolve any citation. ANTS-5058 made that walk
prune noise directories (no more descending into `build/`), but
`IndieReviewDialog::onAllReportsCollected` and
`ColdEyesDialog::onAllReportsCollected` each still call the corroboration
engine **twice** per round — once at `minLanes=2` for the corroborated set,
once at `minLanes=1` for everything, subtracting one from the other by
`(file, line)` key to get the single-lane leftovers. Both calls run
synchronously on the GUI thread.

At the default `lineSlop` (0) and no stats pointer, `minLanes` only filters
which coverage groups become findings — it changes no other output. So a
`minLanes=1` result, split by lane count, is exactly the `minLanes=2` result
plus the leftovers the dialogs were computing with a second full walk.
`IndieReviewEngine::splitByLaneCount` (added for this fix) does that split
without a second walk.

Full item: ANTS-5125.

## Invariants

- **INV-1** — Split parity (guard, holds today). For a fixed project root
  and reports map, `splitByLaneCount(corroboratedFindings(root, reports, 1))`
  equals, field for field (`file`, `line`, `citingLanes`), the pair formed by
  `corroboratedFindings(root, reports, 2)` (the `.corroborated` half) and the
  `minLanes=1` result's findings that are *not* in that `minLanes=2` result,
  keyed by `(file, line)` (the `.singleLane` half). This is the seam the fix
  is built on, not the fix itself — it holds against the code on disk today
  and is expected to stay green.

- **INV-2** — One walk per round (wiring, does not hold yet). Each dialog's
  `onAllReportsCollected` body calls the corroboration entry point exactly
  once — `IndieReviewEngine::corroboratedFindings(` once in
  `IndieReviewDialog::onAllReportsCollected`, `ColdEyesEngine::
  crossDocDiffFromReports(` once in `ColdEyesDialog::onAllReportsCollected`
  — and both bodies call `splitByLaneCount(` to get the corroborated and
  single-lane halves from that one result. Today each body calls its entry
  point twice (once per `minLanes`) and calls `splitByLaneCount` nowhere, so
  this invariant is expected to fail until the dialogs are rewired.

## Test scope

INV-1 is behavioural: a small on-disk fixture project (two lanes citing one
line, a third lane citing a different line alone), exercised through the
real `corroboratedFindings` / `splitByLaneCount` seam — no dialog, no I/O
beyond the fixture.

INV-2 is a source scrape of `src/indiereviewdialog.cpp` and
`src/coldeyesdialog.cpp`, comments stripped, anchored on the qualified
function signature (`void IndieReviewDialog::onAllReportsCollected(` /
`void ColdEyesDialog::onAllReportsCollected(`) rather than the bare method
name — the bare name can match a `connect()` reference before the
definition. A behavioural test can't observe how many times a dialog
walked the tree internally, so a call-count grep is the only way to hold
"once per round".

## Out of scope

- Moving the walk off the GUI thread. The roadmap item's fix is "corroborate
  once at minimum 1 and split the result by lane count" — it does not ask
  for the call to move to a worker thread, and this spec does not invent
  that requirement.
- `ColdEyesDialog`'s loop-log bookkeeping (`m_loopLog`) that reads
  `m_results.corroborated` / `.uncorroborated` sizes after the corroboration
  call — those fields are unchanged in shape by this fix (still
  `QList<CorroboratedFinding>`), so existing behaviour there is untouched.

## Regression history

Introduced when each dialog was first wired to
`IndieReviewEngine::corroboratedFindings` / `ColdEyesEngine::
crossDocDiffFromReports`: getting both the corroborated set and the
single-lane leftovers meant two calls at two `minLanes` values, diffed by
key. ANTS-5058 pruned the walk's noise directories, which made the *cost*
of each call visible — same defect, cheaper each time it fires, still
firing twice. ANTS-5125 adds `IndieReviewEngine::splitByLaneCount` (see
`src/indiereviewengine.h`) so a single `minLanes=1` call can be split
in-memory instead of re-walked.
