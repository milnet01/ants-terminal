# ANTS-1962 — `feedback_log` MCP tool (append to a `*_Ants_MCP_Feedback.md` file) — review loop log

Record for [`../specs/ANTS-1962-feedback-log-mcp-tool-append.md`](../specs/ANTS-1962-feedback-log-mcp-tool-append.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-03)** — 3 cold reviewers (read-verb, write-verb,
  cross-cutting lanes). Verified + fixed: the off-taxonomy refusal codes
  `missing_field`/`bad_op`/`bad_id` were remapped to canonical
  `bad_args`/`bad_mode` (`mcp-error-codes.md` defines neither of the
  former); `not_feedback_file` added to the taxonomy as a justified new
  row; the advisory `—` status declared intentionally unwritable (§ 2.3,
  INV-8); the T8 write-failure seam pinned to a new
  `setForceFeedbackWriteFailForTest` (the existing `g_forceCounterCommitFail`
  is counter-file-only); explicit `callerCwdContractFor` branch added
  (ANTS-1417); `allowOutsideRoot` 5-arg signature cited to
  `pathvalidation.h:46-50`; Status token → `spec draft`; added § 4
  RAM/build cost, § 7 Cross-doc impact, and `*Test:*` clauses on every
  INV; the derived-H1 note clarified against the standard's free-form H1.
- **Loop 2 (2026-06-03)** — cold re-read, 3 lanes. Verified + fixed:
  suffix guard re-pointed to its new canonical home in
  `mcp-feedback-files.md`; `bad_status` broadened in the taxonomy to cover
  write-side status values (§ 7 + `mcp-error-codes.md`); status-emoji cite
  widened to the `§ Status emojis` table + machine-set sentence; the
  `n/a`-only ID-cell rendering declared intentional with
  the standard's parentheticals (`(self-resolved)` etc.) explicitly out of
  scope (recorded via the `notes` column); § 7 reworded to past tense.
  Dismissed (unverified): an off-by-one in the `:7959-7961` cite (grep
  confirms accuracy) and a 1417/1419 mechanism conflation (the
  `setForceFeedbackWriteFailForTest` seam is correctly described as
  net-new; the ANTS-1417 coverage-test wording is faithful to the source
  comment).
- **Loop 3 (2026-06-03)** — cold re-read, 3 lanes, 0 CRITICAL. Fixed three
  line-cite drifts (caused by loop 2's paragraph insertion into the
  standard) by switching to durable section-name references: parentheticals
  → § "Maintainer tracking block"; status-emoji set → § "Status emojis";
  free-form H1 → § "File skeleton". Added INV-14 pinning the success
  envelope `{ok, op, path, bytes_appended, date, created}`; pinned the 4th
  `Notes` column to render empty cells for note-less rows (well-formed
  GFM). Convergence loop.
- **Loop 4 (2026-06-03)** — cold re-read, 3 lanes: **0 CRITICAL, 0 HIGH**
  (clean pass). Remaining MEDIUM/LOW polish folded: T3 fixture pinned to an
  ID-bearing tracking row (so "new IDs in `mapped_ids`" is testable);
  § 2.3 now states an `n/a` row advances the watermark but adds no
  `mapped_ids`; § 2 names ANTS-1961 § 2.2 as the canonical home for the
  union `FeedbackFile` header (anti-drift). Loop closed.

### ANTS-3376 amendment review (2026-06-30)

A fresh 4-loop cold-eyes pass (run alongside ANTS-1961) covering the
default-path derivation (`path` now optional → `<leaf>_…` at the shared
root, `path_derived` echo; INV-15) and the documentation of the
already-shipped `dry_run` arg.

- **Loop 1** — 3 lanes. Reconciled "path required" → optional everywhere;
  added INV-15, T9, and the §2.4 not_found candidate-list note; documented
  the `dry_run` arg (previously asserted by INV-14 but undefined).
- **Loop 2** — restored a dropped `## 4. RAM / build cost` heading; added a
  `dry_run` regression test (T10, `DryRunPreviewCarriesPathDerivedNoWrite`)
  since the arg had no coverage; quoted the literal skeleton H1
  `# Ants MCP Feedback — <Project>` (was a vague placeholder); documented
  the `read_failed` refusal; reordered T9 to numeric position.
- **Loop 3** — corrected the atomicity prose (the seam calls
  `cancelWriting()`, not a failed `commit()`); converted two pre-existing
  drifted line cites (`g_forceCounterCommitFail:3202`,
  `claudeintegration.cpp:7959-7961`) to durable symbol cites.
- **Loop 4** — **0 CRITICAL, 0 HIGH on the feature surface.** Folded:
  corrected the §2.4 `all_other_projects` scoping (leaf-mismatch-gated, not
  derived-path-gated). Verified-clean and dismissed: the `ants_core_lib`
  target claim (accurate — `src/feedbackfile.cpp` is in that
  `add_library`). The dry_run/skeleton/atomicity/derivation contract
  matched the shipped code in every loop.
