# ANTS-4485 — cold-eyes loop log

## Cold-eyes loop log

| Loop | Date | Model | Lanes | Q1 | Q2 | Q3 | Q4 | Verified | Fixed | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2026-09-20 | Opus 5 | 3 | 1 | 2 | 0 | 3 | 6 | 6 | All author's own. All three lanes found § 6's claim that `rlStoreItemPk()` step 2 extracts an id from a headline token; verified false against source — step 2 is headline equality. § 7's red-first precondition was impossible for the three invariants that assert preserved behaviour; now split into changed-behaviour and regression-guard sets. INV-1's fixture needed an `element` row or the render's unfiled-item refusal fails it for an unrelated reason. § 4.5's heading contradicted its own body. |
