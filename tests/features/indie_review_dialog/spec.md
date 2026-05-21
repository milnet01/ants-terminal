# indie_review_dialog — feature-conformance contract

Tracks `docs/specs/ANTS-1258.md` (`IndieReviewDialog : ReviewDialogBase`).
Exercises the non-GUI composition / corroboration / synthesis / fold-in
paths — the engine calls (`IndieReviewEngine`), the false-positive
ledger, and the ROADMAP fold-in — not the live Qt event loop. INV-8 is a
construction smoke assertion.

## Asserts

- **INV-1** — `derivePartition()` returns one `ReviewLane` per
  `IndieReviewEngine::derivePartition(cwd)` entry; a lane deselected via
  `setLaneSelected(name, false)` is absent from the returned set (so the
  base never dispatches a job for it).
- **INV-2** — `composeBrief(lane)` reuses
  `IndieReviewEngine::assembleBriefForDispatch`: source bodies are fenced,
  the standards docs are inlined, and the `ants::falsepos` prior-FP block
  for `("indie-review", lane.name)` appears exactly once (not duplicated
  by the dialog).
- **INV-3** — the composed user prompt is sum-capped at `kPromptCapBytes`
  (200 KiB): an oversize lane is truncated with a marker; a small lane's
  `userPrompt` is byte-identical to `assembleBriefForDispatch(cwd, lane)`.
- **INV-4** — `onAllReportsCollected` corroborates via
  `corroboratedFindings(minLanes=2)`; a `file:line` cited by two lanes
  lands in `results().corroborated`, a single-lane cite in
  `results().uncorroborated` (not dropped).
- **INV-5** — `dispatchSynthesis()` runs the synthesis job through the
  base `dispatchOne` and does NOT re-enter `onAllReportsCollected` (the
  callback sets `synthesisText()`; the `onAllReportsCollected` call-count
  is unchanged).
- **INV-6** — after results, `lanesToReReview()` is exactly the set of
  lanes that produced findings (clean lanes excluded).
- **INV-7** — `performFoldIn()` in `Narrative` mode allocates no IDs
  (`.roadmap-counter` unchanged); in `PerFinding` mode it allocates
  exactly one ID per actionable corroborated finding (counter advances
  by N).
- **INV-8** — constructing the dialog with an unset/invalid `ai_endpoint`
  config leaves dispatch disabled (`endpointDispatchable` false); the
  dialog constructs without crashing.
