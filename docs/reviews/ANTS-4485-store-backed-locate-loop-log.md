# ANTS-4485 — cold-eyes loop log

## Cold-eyes loop log

| Loop | Date | Model | Lanes | Q1 | Q2 | Q3 | Q4 | Verified | Fixed | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2026-09-20 | Opus 5 | 3 | 1 | 2 | 0 | 3 | 6 | 6 | All author's own. All three lanes found § 6's claim that `rlStoreItemPk()` step 2 extracts an id from a headline token; verified false against source — step 2 is headline equality. § 7's red-first precondition was impossible for the three invariants that assert preserved behaviour; now split into changed-behaviour and regression-guard sets. INV-1's fixture needed an `element` row or the render's unfiled-item refusal fails it for an unrelated reason. § 4.5's heading contradicted its own body. |
| 2 | 2026-09-20 | Opus 5 | 3 | 1 | 2 | 1 | 2 | 5 | 5 | VIOLENT CAP. 2 of 5 findings landed on text loop 1 wrote (the id fall-through, and 'reuses step 2'); the rest were original draft text. The decisive one: loop 1's red-first fix NEVER LANDED - the edit's search string did not match the file and failed silently, and the orchestrator reported it fixed. All three lanes quoted the original sentence back. Every loop-1 edit was audited afterwards; that was the only miss. Also corrected: an id miss must NOT fall through to a headline scan (it contradicted INV-2 and would write to an item the caller never named), and line_range is refused only in flip_batch today, so INV-6 is new work in two handlers rather than preserved behaviour. Cap reached; routed to implementation, not a third loop. |
