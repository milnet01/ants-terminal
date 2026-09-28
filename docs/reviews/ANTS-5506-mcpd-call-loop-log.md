# ANTS-5506 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-5506-mcpd-call.md`](../specs/ANTS-5506-mcpd-call.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-28 | 2 (neutral-lane; each held every question) | 3 | 2 | 2 | 2 | 9 verified, 9 fixed, 0 dismissed. Lanes: caller_cwd Required on doc_lint/spec_lint (both lanes); overlapping exit-code rows (both); per-call offload overriding the session default; the INV-3 test unable to fail under the 4096-byte clamp; unstated wrap shapes; stdio-only setup unnamed. Orchestrator, settling open questions: terse default compaction drops an empty findings; a missing path is ok:true for spec_lint, so INV-2 needed a bad_path case; tool_info is inline, so the verb set is registeredToolNames(). |
