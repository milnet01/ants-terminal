# cold_eyes_dialog — feature-conformance contract

Tracks `docs/specs/ANTS-1721.md` (`ColdEyesDialog : ReviewDialogBase`).
Exercises the non-GUI composition / corroboration / fold-in paths — the
engine calls (`ColdEyesEngine`), `BriefDispatch` inlining, the
false-positive ledger, and the ROADMAP fold-in — not the live Qt event
loop. INV-8 is a construction smoke assertion.

## Asserts

- **INV-1** — `derivePartition()` returns one `ReviewLane` per
  `ColdEyesEngine::derivePartition(cwd, scope).lanes` entry; a lane
  deselected via `setLaneSelected(name, false)` is absent from the
  returned set (so the base never dispatches a job for it).
- **INV-2** — `composeBrief(lane)` inlines the lane's doc bodies whole
  (fenced via `BriefDispatch::inlineBodies`), narrows cross-reference
  contracts to keyword-matching sections (`inlineRelevantSections`), and
  includes the `ants::falsepos` prior-FP block when the ledger has a
  matching entry. Irrelevant cross-ref sections are absent.
- **INV-3** — a doc citing a missing `src/x.cpp:NN` surfaces as an
  `[ACCURACY]` stale finding in `results()` even when the lane's model
  report is empty (resolved off-disk by the engine, no model round-trip).
- **INV-4** — the composed user prompt is sum-capped at
  `kPromptCapBytes` (200 KiB): under budget the relevant cross-ref
  sections are present; over budget the lane bodies are retained, the
  cross-ref excerpts are dropped first, and a truncation marker appears.
- **INV-5** — `onAllReportsCollected` corroborates via
  `crossDocDiffFromReports(minLanes=2)`; a `file:line` cited by two
  lanes lands in `results().corroborated`, a single-lane cite in
  `results().uncorroborated` (not dropped).
- **INV-6** — after results, `lanesToReReview()` is the set of lanes
  that produced findings. The re-review brief for one of those lanes runs
  **cold** (ANTS-2011): `composeBrief` carries no "Previously fixed in a
  prior loop" block. A finding that comes back is the signal that the
  earlier fix didn't hold — nothing tells the reviewer to skip it. (Not
  tested by absence of the string "re-raise" alone: the unrelated prior-FP
  block INV-2 requires legitimately contains that word too.)
- **INV-7** — `performFoldIn()` in `Narrative` mode allocates no IDs
  (`.roadmap-counter` unchanged); in `PerFinding` mode it allocates
  exactly one ID per actionable corroborated finding (counter advances
  by N).
- **INV-8** — constructing the dialog with an unset/invalid `ai_endpoint`
  config leaves dispatch disabled (`endpointDispatchable` false); the
  dialog constructs without crashing.
- **INV-9** — each **dispatched round** — a full dispatch
  (`startDispatch()`) or a re-review (`redispatch()`, via the "Re-review
  lanes with findings" button) — appends exactly one `LoopEntry` to
  `loopLog()` (ANTS-2011): `loop` is the 1-based round number; `lanes` is
  the lanes **that round dispatched**, sorted; `corroborated` /
  `uncorroborated` are `results().corroborated.size()` /
  `results().uncorroborated.size()` after that round's collection. A
  re-review merges its reports into the earlier ones
  (`ReviewDialogBase::redispatch` accumulates into `reports()`, and
  `onAllFinished` always hands `onAllReportsCollected` the whole merged
  map), so the counts cover every lane's latest report — but `lanes`
  is *not* `reportsById.keys()`: it is the set that round actually
  dispatched, tracked separately (e.g. via `prepareDispatch()` for a full
  dispatch, and the re-review's own lane set), or a re-review's entry
  would wrongly list every lane ever dispatched instead of just the ones
  re-checked. Exactly one entry is appended per round. A call to
  `onAllReportsCollected` with no round pending appends nothing. The log is
  also rendered into a `QPlainTextEdit` results view, one line per round,
  each containing the text `"Round N"`.
