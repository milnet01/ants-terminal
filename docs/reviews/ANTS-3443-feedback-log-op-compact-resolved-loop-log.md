# ANTS-3443 — `feedback_log op:"compact_resolved"`: collapse a shipped v2 finding's write-up to a roadmap-driven stub — review loop log

Record for [`../specs/ANTS-3443-feedback-log-op-compact-resolved.md`](../specs/ANTS-3443-feedback-log-op-compact-resolved.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-07-05)** — 2 reviewers, cold; cited code (feedbackfile.h/.cpp,
  remotecontrol.cpp, roadmapindex/roadmapdialog) + standard + sibling specs.
  0 CRITICAL. Verified + fixed: 1 HIGH, 6 MEDIUM, 3 LOW, 1 INFO. H1 — `n/a` /
  id-less-block classification was wrong: the spec labelled an `n/a` closure and
  an id-less `### ` block `untriaged`, but the standard makes `n/a` a *triaged
  closure* (never compacted, even when it names an id) and an absent Proposed-ID
  line a *non-finding* (not enumerated, not in `skipped[]`) → renamed the skip
  code `untriaged`→`no_shippable_id`, added closure-wins, made id-less blocks
  non-findings, corrected the §2.10 example + INV-12. M's — cited the standard's
  exact id-line regex + "first-line-wins"; documented archive-rotation as
  `roadmap_unresolved_ids` (§2.5/§5); repointed stale `remotecontrol.cpp`
  citations (10757/10768/10769 → 10832/10847/10849); added the gate-order
  rationale; flagged (and fixed in the standard) the `→ shipped ANTS-NNNN` vs
  `→ shipped ✅ (…)` stub-text drift at standard lines 87/199. L's — `line`
  1-based; fence-safe idempotency probe; signed-`bytes_saved` note.
- **Loop 2 (2026-07-05)** — 2 reviewers, cold. 0 CRITICAL. Verified + fixed:
  1 HIGH (self-inflicted), 2 MEDIUM, 4 LOW. H1 — §7(d) told the implementer to
  reconcile the stub-text drift that loop 1's own standard edit had *already*
  fixed → removed the phantom instruction, switched §7 to drift-proof
  section-heading citations. M — the "enumerator reused verbatim, no drift"
  claim was overstated: `feedback_query` needs a *ternary* classification
  (`suspected_untagged[]`) the binary compaction scanner lacks → softened §2.6/§7
  to "share the scanner, layer classification." L — specified `skipped[].ids`
  semantics; added `not_feedback_file` to INV-11; filtered empty-id bullets from
  `roadmapIds` (§2.3, `isCanonicalId`); §2.4 v1-file wording corrected.
- **Loop 3 (2026-07-05)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH. Verified +
  fixed: 2 MEDIUM, 3 LOW. M1 — §2.5 claimed the id-line matcher is "the exact
  matcher `parse()` uses", but `parse()` is v1-only today (no Proposed-ID
  matcher) → reworded to a target the spec + pending v2 parser MUST share. M2 —
  the committed standard's gate-1 predicate ("holds ≥ 1 id") still read as
  collapsing an `n/a` closure → reconciled the standard's gate list in place
  (rename, reorder `already_compacted` before roadmap gates, closure-wins,
  id-less-non-finding), and updated the standard's "provisional codes" caveat.
  L — added "(case-insensitive)" to the `n/a\b` test (gate 1 + INV-2); annotated
  the v1-framed ROADMAP ANTS-3443 bullet to point at this spec.
- **Loop 4 (2026-07-05)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH. Verified +
  fixed: 1 MEDIUM, 3 LOW. M — §7 said "confirm `not_v2` is present" in the error
  taxonomy, but neither `not_v2` nor `roadmap_unavailable` is registered →
  reworded to "add both." L — noted ANTS-3442's row-dedup half already shipped
  and flipped its bullet ✅ (this op is its sibling half); corrected the INV-1
  guard line to 10832-10841; added the missing post-heading blank to the
  standard's stub-order prose. The reviewer independently re-verified every code
  citation and the full spec↔standard gate-list consistency.
- **Loop 5 (2026-07-05)** — 1 reviewer, cold. 0 CRITICAL / 0 HIGH. Verified +
  fixed: 1 MEDIUM, 1 LOW, 1 INFO. M — the standard pre-declared the spec
  "cold-eyes'd" while the spec log still read "pending" (a status contradiction I
  created in loop 3) → standard reworded to "cold-eyes reviewed" and this log
  written. L — pinned "unresolved before open" in the standard's gate 3 to match
  §2.5/INV-7. INFO — added the 1-based `line` to the `skipped[]` envelope for
  parity with `collapsed[]`. Reviewer verdict: "implementation-ready on its
  technical merits."
- **Loop 6 (2026-07-05)** — 1 reviewer, cold. **0 CRITICAL / 0 HIGH / 0 MEDIUM /
  0 LOW — clean pass.** Every code citation re-verified live
  (`remotecontrol.cpp` guard 10832-10841, dispatch 10847/10998, fbNotFound
  10849, parseBullets 449; `feedbackfile.cpp` 55/72/345; `isCanonicalId`;
  `BulletRecord{id,status}`); spec↔standard agreement confirmed on all four
  gate-list dimensions (regex, order, n/a-closure-wins, id-less-non-finding);
  the §2.10 example arithmetic and INV-1…INV-13 traced sound; the
  marker-fallback (read) vs `not_v2` (write) apparent tension confirmed a
  non-contradiction. One INFO (non-blocking): `compact_resolved`'s value is
  gated on `assign_id` + `migrate_v2` existing to produce v2 files — a build-
  order note, not a spec defect.
- **Convergence (2026-07-05):** 6 loops, 2→2→1→1→1→1 reviewers, all cold.
  1 HIGH (loop 1, the `n/a`/id-less classification) drove the substantive
  rework; the loop-2 HIGH was self-inflicted by loop 1's own standard edit and
  did not reopen a contract issue. Loops 3–6 were 0-CRITICAL/0-HIGH with a
  shrinking residual (spec↔standard reconciliation, citations, wording), and
  loop 6 returned zero verified findings across all severities. Signed off as
  implementation-ready — the shared `### ` scanner + the id-line regex are the
  only cross-spec commitments the implementer must honour (§2.6/§7).
- **impl — ANTS-6985 doc fix (2026-11-28), no reviewer dispatched** — The Proposed-ID passage now points at mcp-feedback-files.md's id-line parser contract instead of quoting an old regex.
