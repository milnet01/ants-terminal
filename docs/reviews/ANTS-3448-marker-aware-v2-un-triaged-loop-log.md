# ANTS-3448 — marker-aware v2 un-triaged delta in `feedback_query` + `session_orient feedback_pending` — review loop log

Record for [`../specs/ANTS-3448-marker-aware-v2-un-triaged.md`](../specs/ANTS-3448-marker-aware-v2-un-triaged.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two reviewers per loop, cold (loop N+1 briefed identically to loop 1 — never
told what was fixed).

- **Loop 1** — 0C, 1H, 6M, ~7L (deduped across both lanes). Both lanes verified
  the load-bearing structural claims **sound**: INV-6 (the compaction ops read
  only `lastMaintainerLine`/`trackingRows`, never `delta*`/`mappedIds`), §2.4
  (`feedback_pending` reads only `deltaPresent`/`deltaLineCount` → no code
  change), and `mappedIds` is consumed only by `cmdFeedbackQuery`. **H:** the
  §2.5 worked example's v1 contrast was false — its post-table headings were all
  `### ` (inert to the v1 `#`/`## ` scanner), so v1 would yield an *empty* delta,
  not "all four blocks"; reworked the fixture with a `## ` session heading + a
  real `ANTS-1400` table id so both the v1-over-broad contrast and the
  "retained-table id excluded" point actually hold. **M:** (a) `>= 2` vs the
  standard's "iff `: 2`" — reconciled by amending the standard's Version-selection
  to degrade-forward `>= 2` (§7); (b) `mappedIds` "first non-`n/a` line" wording
  contradicted the first-id-line-wins scanner — restated as "first id line,
  contributed only when not an `n/a` closure"; (c) the v2 `delta` is a
  concatenation of non-contiguous findings, so `deltaStartLine` is **not** a slice
  anchor — added INV-11 + a `feedbackfile.h` comment update + a ≥2-finding test;
  (d) trailing-blank handling per v2 block was undefined (→ trim, matching v1);
  (e) `claudeintegration.cpp` description **does** enumerate outputs (so the
  update is mandatory + the stale v1 summary needs rewording) — made §7 definite;
  (f) marker detection "its own regex" invited read/write drift → mandated a
  shared `markerVersion` helper (INV-12). **L:** step-2 lead-in undersold the
  rule (any non-id/non-`n/a` text is un-triaged); reuse `compactResolved`'s
  closure regex; existing-fields list add `ok`/`path`/`path_derived`; shared
  Pass-2 loop framing. All verified fixes applied.

- **Loop 2** — **0C, 0H** (both lanes); every loop-1 fix re-verified sound
  (feedback_pending no-change, INV-6, the reworked §2.5 example, mappedIds
  first-line-wins, fence-inertness, all field names — confirmed against code).
  Residual was **mechanical precision on the loop-1 fixes themselves**: (M1, both
  lanes) the `markerVersion` helper as specified (int only, absent≡malformed≡0)
  couldn't drive `migrate_v2`'s insert-vs-repair branch — scoped the refactor so
  `migrate_v2` keeps its own presence + line-index scan and shares only the
  version extraction (INV-12); (M2, both) the "reuse `compactResolved`'s closure
  regex" / "`migrate_v2`'s `bulletArmRe`" claims were infeasible (function-local
  statics) — prescribed **hoisting both to anon-namespace helpers** (§7); (M3,
  both) the per-block trailing-blank trim was mis-analogised to the v1 single-trim
  and left one-vs-all ambiguous — pinned to "strip **all** trailing blanks per
  block"; (M2 lane B) INV-11 over-claimed (a single un-triaged finding **is** a
  contiguous slice) — softened to "not guaranteed"; (M4/L2) the classification
  cite moved to §"Maintainer triage" (the authoritative value rule) with a
  standard step-2-gloss reconciliation added to §7; conditional-field wording
  (`path_derived`/`tracking`); a closure-with-embedded-id `mapped_ids` oracle
  added to §6. All verified fixes applied.

- **Loop 3** — **0C, 0H** (both lanes); every loop-2 fix re-verified against code
  (`closureRe`/`bulletArmRe` confirmed function-local statics; the v1 single-trim;
  the interleaved Pass-2 loop; the reworked §2.5 example; `feedback_pending`
  no-change; all field names byte-exact). Both lanes surfaced the **same** single
  MEDIUM: `compact_resolved`'s gate is exact `== 2`, which diverges from the
  reader's `>= 2` degrade-forward (a `: 3` file reads as v2 but is refused
  `not_v2` by the compactor) — a **cross-verb rationale** point, not a defect in
  the reader; documented as a deliberate exception (a compactor must not mutate an
  unrecognised future format) in §2.1 + §7. Plus LOW/precision: "marker line
  index" → "re-locates the marker line"; INV-8/§2.3 "every reply" → "every
  **success** reply"; a two-id-union `mapped_ids` oracle, an INV-12
  migrate→parse round-trip case, and an INV-10 purity case added to §6; a §4
  clause on the compaction ops paying the v2 pass. All applied.

- **Convergence (2026-07-05):** 3 loops. Loops 2 and 3 both returned 0C/0H;
  the loop-3 residual was one cross-verb documentation point + wording precision
  → **polish-converged** (no structural/mechanical/architectural finding). Every
  load-bearing claim was re-verified live across the loops: INV-6 (compaction ops
  read only `lastMaintainerLine`/`trackingRows`), §2.4/INV-9 (`feedback_pending`
  needs no code change), the v2 rule matches the standard's §"Maintainer triage"
  value rule, `mappedIds` first-id-line-wins + closure-contributes-zero, fence
  inertness, the reworked §2.5 example's v1/v2 contrast, and the
  function-local-static regex hoists. Signed off as implementation-ready; the
  implementer's cross-doc commitments are the §7 items (the `markerVersion` +
  regex hoists, the `ParseResult` comment update, the `claudeintegration.cpp`
  description update, and the three standard amendments).
