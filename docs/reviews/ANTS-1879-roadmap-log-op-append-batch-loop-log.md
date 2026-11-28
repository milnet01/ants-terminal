# ANTS-1879 — `roadmap_log op:append_batch` verb — review loop log

Record for [`../specs/ANTS-1879-roadmap-log-op-append-batch.md`](../specs/ANTS-1879-roadmap-log-op-append-batch.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-26) — joint review with ANTS-1878

Two reviewers, one per spec, briefed against both specs + cited code.

**CRITICAL — fixed.**
- File-ref error: spec pointed at `src/mainwindow.cpp` for the
  `roadmap_log` schema, but the description + schema actually live
  in `src/claudeintegration.cpp:5669+`. § 2.6 + § 7 now cite the
  correct path.

**HIGH — fixed.**
- `all_skipped` divergence from `flip_batch`: spec claimed
  `ok:false, code:"all_skipped"` for the empty-result case, but
  `cmdRoadmapLogFlipBatch` at `remotecontrol.cpp:4244-4256`
  returns `ok:true` with empty arrays. Dropped `all_skipped`;
  § 2.4 + INV-4 now mirror flip_batch's `ok:true` shape and
  expose `applied_count` / `skipped_count` for the caller to
  branch on.
- New handler + test seam declarations were implicit. § 2.1 now
  shows them verbatim.

**MEDIUM — fixed.**
- `formatRoadmapBullet` signature was missing the `scrubbedNames`
  out-param (the existing block at `remotecontrol.cpp:3314-3318`
  fills it). § 2.2 step 5 + INV-10 now pin the four-arg signature.
- Gate ordering (counter-state vs per-bullet validation) was
  unspecified. § 2.2 now sequences counter/format/section gates
  before per-bullet validation.
- `id_hint` on bullets[1..] was unspecified. § 2.1 + INV-6 now
  state silent-ignore.
- `4010` cite for flip_batch was at the function definition; doc
  comment is at `:3996`. § 1 now cites the doc-comment range.

**LOW — fixed.**
- "read side" → "write side" in the Pairs-with header.
- Per-bullet warning aggregation (response-level `warnings`
  dedup) was unspecified. § 2.5 now states the dedup contract.

### Loop 2 (2026-05-26) — fresh reviewer, cold re-read

**HIGH — fixed.**
- § 7 cross-doc impact called out "no new codes" without
  acknowledging the pre-existing taxonomy gap on inherited codes.
  Restated: ANTS-1879 co-owns the catch-up of `bad_case` with
  ANTS-1878 § 7; the remaining inherited codes (`headline_empty`,
  `bad_kind`, `id_taken`, `counter_*`, `stable_prefix_unsupported`,
  `roadmap_write_failed`) are flagged as pre-existing debt, out of
  scope here. Avoids the "nothing to do" reading.
- § 2.2 step 3 was ambiguous on "empty bullets" — could mean empty
  bullets-array OR an individual all-empty bullet object.
  Disambiguated: array-empty is gated in step 1; an individual
  bullet with every required field missing joins `skipped[]` with
  `code:"missing_field"` like any other per-bullet failure.

**MEDIUM — fixed.**
- § 2.1 cite `remotecontrol.h:617-621` was loose (617 is
  `cmdRoadmapLogFlipForTest`, not `FlipBatch*`). Narrowed to
  `:621` (`cmdRoadmapLogFlipBatchForTest`).
- `applied_count` vs `flipped_count` field-name asymmetry was
  unmotivated against the "parity with flip_batch" claim. § 2.4
  now states the asymmetry is intentional — semantic parity, not
  byte-for-byte — and that an implementer should NOT rename.
- INV-10 source-grep was brittle (literal `formatRoadmapBullet(`
  matches declaration + definition + call sites). Test surface
  scoped to `src/remotecontrol.cpp` and pinned at exactly 2 call
  sites.
- INV-8 implied a both-sided rollback test, but
  `setForceCounterCommitFailForTest` only drives the counter-fail
  leg. Restated to be one-sided by design, with the same gap
  ANTS-1878 INV-8 acknowledges.

**LOW — fixed.**
- § 1 wording "able to fail mid-batch and leave the caller
  half-applied" was unclear that single-bullet calls are each
  atomic. Restated as "individually transactional but the batch
  as a whole can fail at bullet K leaving K-1 ids consumed".

### Loop 3 (2026-05-26) — fresh reviewer, cold re-read

**CRITICAL — fixed.**
- § 2.1 example slug was `phase-106-multi-threading-concurrency`
  but the slugifyHeading transform converts the `.` in `10.6` to a
  `-`, producing `phase-10-6-multi-threading-concurrency` (matching
  the ANTS-1878 § 2.2 canonical example). Fixed.

**HIGH — fixed.**
- `skipped[]` row schema (`bullet_index`) vs flip_batch's
  `locator_index` (`remotecontrol.cpp:4118`) was an undeclared
  field-name asymmetry. § 2.4 now pins `bullet_index` explicitly
  and states an implementer must NOT rename it to `locator_index`
  during a "parity with flip_batch" pass.

**MEDIUM — fixed.**
- INV-3 didn't tie `bytes_written` to the file delta — implementer
  could read partial success as needing to count all submitted
  bullets. INV-3 now states `bytes_written` covers ONLY accepted
  blocks.
- INV-10 source-grep "exactly 2 call sites" was brittle against a
  legitimate future caller (a third `op` reusing the helper).
  Loosened to "≥ 2 named sites required".

**LOW — fixed.**
- § 2.1 `id_hint` example value `1900` was uncomfortably close to
  the real ID range. Bumped to a clearly-synthetic `99000`.

### Loop 4 (2026-05-26) — fresh reviewer, cold re-read (joint pass)

Cross-spec consistency loop reviewing both ANTS-1878 + ANTS-1879
together. No CRITICAL findings. HIGH/MEDIUM polish folded:

**HIGH — fixed.**
- INV-10 wording was load-bearing for the dedup story but read as
  if the helper already existed. Restated as "implementer MUST
  extract … (new code introduced by this spec — not currently
  present)" so the contract is unambiguous as a refactor
  obligation.

**MEDIUM — fixed.**
- INV-3 lumped three per-bullet failure modes (`headline_empty`,
  `bad_status`, `bad_kind`) under one bullet. Expanded the test
  surface to one case per code so each contract is independently
  named.
- Sibling ANTS-1878 § 7 catch-up to `mcp-error-codes.md` now
  includes `no_roadmap` alongside `bad_case`; ANTS-1879 inherits
  both.

The two specs are now considered implementation-ready (spec draft
→ ready for sign-off). Loops 1–4 have converged on a stable
surface: every CRITICAL/HIGH finding raised across the four loops
has been fixed and not resurfaced cold.

### amend-6640 (2026-11-28)

Not a review loop; no reviewer was dispatched (user decisions 2026-11-28, ANTS-6640 row 118 and ANTS-6641 row 101). An id_hint more than 10^6 above the highest id skips bad_args; a second bullet naming a pass an earlier bullet took skips id_taken on the file route too. Tests seen red on the old code, then green.
