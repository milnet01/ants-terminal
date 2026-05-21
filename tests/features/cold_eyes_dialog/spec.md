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
  that produced findings; `markFindingFixed(f)` threads the fix into the
  next `composeBrief` as a "do not re-raise" prior-fix line.
- **INV-7** — `performFoldIn()` in `Narrative` mode allocates no IDs
  (`.roadmap-counter` unchanged); in `PerFinding` mode it allocates
  exactly one ID per actionable corroborated finding (counter advances
  by N).
- **INV-8** — constructing the dialog with an unset/invalid `ai_endpoint`
  config leaves dispatch disabled (`endpointDispatchable` false); the
  dialog constructs without crashing.
