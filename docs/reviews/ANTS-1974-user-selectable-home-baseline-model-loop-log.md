# ANTS-1974 — User-selectable home/baseline model tier — review loop log

Record for [`../specs/ANTS-1974-user-selectable-home-baseline-model.md`](../specs/ANTS-1974-user-selectable-home-baseline-model.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-04):** 0 CRITICAL, 2 HIGH, 3 MEDIUM, 2 LOW — all verified
  and folded. H1 floor value-domain + one-sided clamp pinned; H2 envelope key
  `"home"`→`"home_tier"` (snake_case precedent); M1 epoch comment append-only
  note; M2 memo key spelled `(mtime, size, homeTier)` storing the post-band
  Result; M3 test bundle corrected `test_claude`→`test_core` (verified in
  CMakeLists.txt); L1 enum-cast arithmetic note; L2 this log. Open questions
  resolved: floor stays `{haiku,sonnet}` (no widen); setter follows
  `storeIfChanged+save`. Findings were accuracy/clarity polish (no design
  change), so per user direction implementation proceeds without a 2nd loop.
