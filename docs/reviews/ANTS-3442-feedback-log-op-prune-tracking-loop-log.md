# ANTS-3442 — `feedback_log op:"prune_tracking"`: drop superseded duplicate maintainer tracking rows — review loop log

Record for [`../specs/ANTS-3442-feedback-log-op-prune-tracking.md`](../specs/ANTS-3442-feedback-log-op-prune-tracking.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-07-04)** — 1 reviewer, cited code (feedbackfile.h/.cpp,
  ANTS-3421-feedback-log-op-compact-shipped.md, mcp-feedback-files.md). Verified findings: 1 CRITICAL, 1 HIGH,
  4 MEDIUM, 3 LOW (all fixed). C1 — `TrackingRow` carries no source line, so
  row removal was unimplementable as written → §2.2 now adds the additive
  `int line` on `TrackingRow`. H1 — INV-5 "`lastMaintainerLine` unchanged" was
  numerically false (removing rows above the final heading decreases it) →
  restated as delta-text byte-identity. M1 — no `mappedIds`-preservation
  guarantee (a notes-cell id could be silently unmapped) → §2.3 clause 3 +
  INV-2b. M2 — INV-9 idempotency justification wrong for multi-id rows →
  corrected (no *surviving* row is fully superseded). M3 — reconcile with
  ANTS-3421 §5 (never compact maintainer *blocks*) → §4.1 carve-out. M4 — §7
  wrongly said the parent bullet flips ✅ on ship → parent stays open until
  both halves ship. L1 — request arg `ids` collided with a row's `ids` →
  renamed `scope_ids`. L2 — INV-1 routing corrected to an op-mode inside
  `cmdFeedbackLog`. L3 — added the §2.7 before→after worked example.
- **Loop 2 (2026-07-04)** — 1 reviewer, cold. All loop-1 citations
  re-verified accurate; 0 CRITICAL. Verified findings: 1 HIGH, 1 MEDIUM, 3 LOW
  (all fixed). H1 — §2.3 clause 3 was circular ("surviving" defined by the
  selection itself): two rows each citing an id `X` only in their notes could
  both be removed, dropping `X` → restructured as an explicit two-stage
  fixpoint (Stage-1 marks by id-column supersession; Stage-2 removes a marked
  row only if every id-token also appears in a line NOT marked in Stage 1) +
  INV-7 reworded + a dedicated §6 test case. M1 — §7 didn't flag amending
  mcp-feedback-files.md:314/326 ("don't edit a maintainer block" / "one
  sanctioned exception") → added. L1 — clause-3 reference set aligned to
  `mappedIds`'s per-*line* semantics. L2 — added a spec-id↔bullet reconciliation
  line. L3 — `removed[]` noun `id_list` → `ids`. Also stated the parse
  assumption (contributor tables never enter `trackingRows`).
- **Loop 3 (2026-07-04)** — 1 reviewer, cold. 0 CRITICAL, 0 HIGH; all
  citations re-verified accurate. Verified findings: 3 MEDIUM, 4 LOW (all
  fixed). M1 — INV-2b cited "§2.3 clause 3" but notes-cell pinning moved to
  Stage 2 → corrected to "§2.3 Stage 2". M2 — §2.6/INV-9 idempotency reason
  ("no surviving row is fully superseded") was false for Stage-2-pinned rows →
  restated (survivor is unmarked OR pinned, stable on re-run). M3 — §7 wanted
  to amend mcp-feedback-files.md:314, but that line is a *contributor*
  prohibition (`## Contributor don'ts`) → amend only line 326 (the maintainer
  compaction section). LOW: "fixpoint" → "two-stage pass" (no iteration); §6
  INV-2b fixture now states the required superseded-id-column setup;
  emptied-table column-drift documented as a v1 non-goal; `removed[].status`
  confirmed intentional.
- **Loop 4 (2026-07-04)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH / 0 MEDIUM;
  all citations + the two-stage logic + §2.7 example + idempotency re-verified
  sound. 2 LOW fixed: L1 — §2.5 envelope now echoes `path_derived` (parity with
  compact_shipped / ANTS-3376); L2 — an explicitly empty `scope_ids:[]` is now
  `bad_args` (INV-12), disambiguated from an omitted field. INFO folded: §4.1
  now states the ledger-narrowing (status-transition rows intentionally dropped,
  history in git) crisply for the §7 mcp-feedback-files.md:326 amendment.
- **Loop 5 (2026-07-04)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH; algorithm,
  idempotency, §2.7 example, `mappedIds` preservation, and all citations
  re-verified sound. 1 MEDIUM + 1 LOW fixed (wording only, no logic change):
  M1 — INV-6 / §2.3 clause 3 said a scoped prune "never references an
  out-of-scope id," but scoping reads only the ID *column* (notes-cell ids are
  handled by Stage 2) → reworded to "ID column"; L1 — "identical line 47" →
  "parallel line 47". INFO folded: §2.2 states the 0-based→1-based `li + 1`
  conversion; §2.3 documents the Stage-2 conservatism as intentional (protects
  idempotency — don't re-add survivors mid-pass).
- **Loop 6 (2026-07-04)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH; algorithm,
  idempotency, §2.7 example, INV-6 scoping, and all citations re-verified sound.
  1 MEDIUM + 1 LOW fixed: M1 — absent-file behaviour was unspecified (the
  §2.5 "no-op, not a refusal" wording risked an absent file returning a false
  success) → added INV-13 (`not_found`, parity with `compact_shipped`
  `remotecontrol.cpp:10769-10773`) + the `scope_ids`-id-not-in-file no-op line;
  L1 — INV-2 summary prose ("a row whose id has a later duplicate") could read
  as contradicting INV-3 for multi-id rows → tightened to "**all of whose** ids
  have later duplicate rows."
- **Convergence (2026-07-04):** the contract — two-stage removability,
  `mappedIds` preservation (INV-2b), idempotency, delta-text invariance (INV-5),
  and every file:line citation — was independently verified sound across FOUR
  consecutive cold passes (loops 3–6, all 0 CRITICAL / 0 HIGH). Each loop's
  residual was a shrinking summary-prose-precision or completeness-edge finding
  (empty `scope_ids`, absent file, notes-cell scoping wording), fixed as found;
  none reopened a contract issue. Signed off as implementation-ready — a further
  loop would chase wording with no contract risk.
