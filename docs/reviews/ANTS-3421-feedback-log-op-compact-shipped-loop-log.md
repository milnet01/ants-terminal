# ANTS-3421 — `feedback_log op:"compact_shipped"`: collapse a confirmed-shipped finding write-up to a one-line stub — review loop log

Record for [`../specs/ANTS-3421-feedback-log-op-compact-shipped.md`](../specs/ANTS-3421-feedback-log-op-compact-shipped.md) § Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-07-02)** — two independent cold reviewers. Verified + fixed:

- **C1 (CRITICAL)** — §1.1 sold the `id` as the wrong-block guard and quoted
  a *fabricated* tracking row (`| #3 … | ANTS-1567 | ✅ |`; the real row at
  `MAME_Curator…:770` is `(self-resolved)`, id-less). Reframed: the exact
  **heading** locates the block; the `id` is the shipped-gate + breadcrumb,
  trusted-per-maintainer. Corrected the quote; cite by heading not line.
- **C1′ (CRITICAL)** — INV-6 asserted an unchanged numeric
  `lastMaintainerLine`, which the collapse necessarily decreases
  (`feedbackfile.cpp:103` = absolute index). Restated INV-6 as `mappedIds` +
  `delta`-text preservation; numeric line no longer asserted.
- **H1 (HIGH)** — id-less closures (the dominant closed form) can't satisfy
  the id-✅ gate. Scoped v1 to ID-tracked findings; id-less deferred (§5).
- **M (MEDIUM ×5)** — ✅ matched by U+2705 **containment** (not equality);
  idempotency keys on the **first non-blank** body line; gate order pinned
  (top-to-bottom, first failure wins, INV-12); added the `title_block` gate
  protecting the H1/banner (INV-11); `bytes_saved` restated as a **signed**
  sum (INV-9).
- **LOW** — cite corpus by heading not mutable line; dropped dead "at" from
  the `in_delta` gate; `id` membership defined as `∈ TrackingRow::ids`; byte
  fields defined to span the whole block; refreshed the standard's stale
  `~3,460`→`~3,671` line-count.

**Loop 2 (2026-07-02)** — two fresh cold reviewers, briefed exactly as loop
1 (no mention of the above). Both independently confirmed the loop-1
CRITICALs are genuinely fixed. New verified + fixed:

- **H1 (HIGH)** — "next `^#`/`^##` boundary" (§2.2) could be misread as
  same-or-higher-level markdown nesting, letting a `#` target scan past the
  watermark. Pinned to "next boundary of **either** level", byte-identical to
  `parse()` pass-2 (`feedbackfile.cpp:139-147`); added the partial-collapse
  note (a `#` session with `##` children collapses only its preamble;
  tool-authored `###`-only sessions collapse whole).
- **M (MEDIUM)** — duplicate/colliding targets now refused `duplicate_target`
  (new INV-13); gate 5 tightened from ✅-**containment** to ✅-**leading
  token** (so `❓ (not ✅ yet)` can't false-match); net-negative / bodyless
  collapses defined as **applied by design** (normalisation), with
  `bytes_before` = heading bytes for a bodyless block.
- **LOW** — INV-1 now cites the exact §2.2 shape; INV-6 also asserts
  `trackingRows` unchanged; §2.1 example no longer self-contradicts gate 1
  (dropped the mismatched `heading_line`); heading match defined
  trimmed-vs-trimmed; gate 1 covers a non-boundary `heading_line`; §7's
  now-done line-count refresh reworded; §2.6 flags the extraction's
  no-regress constraint on `mappedIds`.

**Loop 3 (2026-07-02)** — two fresh cold reviewers. **Zero CRITICAL, zero
HIGH** — the design has converged; both confirmed the prior loops held.
Remaining verified MEDIUM/LOW clarifications fixed:

- **M (MEDIUM)** — pinned `duplicate_target` precedence (runs over gate-1
  survivors, wins over gates 2–6); stated the wrapper splits
  `CompactResult::outcomes` into envelope `outcomes` (applied) + `skipped`
  (refused).
- **LOW** — named the malformed-input (`bad_args`) and missing-file
  (`not_found`) refusals; echo `path_derived` for sibling-op parity;
  "preserving"→"ensuring" one trailing blank; noted tool-authored ✅ rows are
  exact single-emoji (leading-token only matters for hand-authored cells);
  added the load-bearing note that `mappedIds` is maintainer-body-only (the
  breadcrumb's own id is never counted); broadened §2.6's extraction surface
  to the `{line, text, isMaintainer}` boundary list; added a cross-session
  write-isolation out-of-scope note (pre-existing to all `feedback_log` ops).

**Loop 4 (2026-07-02)** — two fresh cold reviewers. One **HIGH** + verified
MEDIUM/LOW, fixed:

- **H1 (HIGH)** — a multi-finding block (`renderFindingBlock` emits N
  findings as `###` under one `##`/`#` boundary, `feedbackfile.cpp:239`)
  would be collapsed **whole**, erasing the write-ups of still-open sibling
  findings and misattributing them to the one named `id`. Added gate 6
  (`multi_finding`, refuse a block with ≥2 `###` findings) + INV-14; deferred
  multi-finding collapse to a v2 (§5).
- **M (MEDIUM)** — added the supersession INV-15 (a reopened id, ✅-then-📋,
  refuses `not_shipped` — the last row wins); validate `id` matches
  `^ANTS-[0-9]+$` (`bad_args`), which also keeps the `→ shipped ANTS-`
  idempotency sentinel valid.
- **LOW** — INV-12 now carves out `duplicate_target` precedence; §2.5 prose
  lists `end_line`; §2.2/INV-1 define the trailing blank at EOF; renamed the
  helper field `CompactResult::outcomes` → `results` (envelope `outcomes` is
  applied-only) to kill the two-membership foot-gun.

**Loop 5 (2026-07-02)** — two fresh cold reviewers. **0 CRITICAL, 0 HIGH**
(second consecutive clean-of-severity pass — design converged). Remaining
verified MEDIUM/LOW were edge-case hardening of the loop-4 `multi_finding`
gate, all fixed:

- **M** — documented that gate 6's `### `-count reliably guards only the
  tool-authored `renderFindingBlock` shape; hand-authored non-`###`
  multi-finding blocks fall back to maintainer + `dry_run` (byte-ratio
  backstop noted as a follow-up); §2.6 now states gate 6 re-runs the
  file-local fence scanner over the block body.
- **LOW** — INV-6 byte-identity qualified with the fence-balanced
  precondition; `target_ambiguous` now defined to carry a `candidates[]`
  array; INV-5 test extended to the non-boundary `heading_line` case; INV-7
  notes gate-5 supersession can pre-empt `already_compacted` (block still
  byte-unchanged); `### ` over-refusal documented as a safe false-positive.

**Cap reached (5 loops).** Loops 4 + 5 each returned 0 CRITICAL / 0 HIGH; the
tail was documentation hardening of edge cases, now folded. Design is
signed-off — spec **accepted**. Any further edge-case polish is INFO-level
and can ride a later pass.
