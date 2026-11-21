# ANTS-3447 — `feedback_log op:"assign_id"`: v2 inline triage write — review loop log

Record for [`../specs/ANTS-3447-feedback-log-op-assign-id.md`](../specs/ANTS-3447-feedback-log-op-assign-id.md) § Cold-eyes loop log.

## Cold-eyes loop log

Two reviewers per loop, cold (loop N+1 briefed identically to loop 1 — never
told what was fixed).

- **Loop 1** — 0C, 1H, 6M, 5L (deduped across both lanes). **H (both lanes):**
  §7 claimed `target_not_found`/`target_ambiguous` were "already registered by
  `compact_shipped`" — verified **false** (zero hits in `mcp-error-codes.md`);
  the codes are emitted by ANTS-3421 code but never entered the taxonomy. Fixed
  by registering both (category 2) + rewording §7. **M:** (a) the
  assign-vs-close predicate was ambiguous for `ids:[]` + `closure` — replaced
  with an explicit `hasIds XOR hasClosure` rule; (b) `heading` input format
  (marker-included?) unstated — pinned to the full `### ` line verbatim; (c) a
  free-text `closure` could inject a newline and break INV-3 — added a
  fold-to-space guard (+ INV-5, + test); (d) the standard's `assign_id` row
  carried an impossible `duplicate_target` (single-target) and a `#`/`## `
  gate-inheritance phrasing — corrected the row to gate-*shape* over the `### `
  enumerator. **L:** `bytes_delta` −16→−15 (24-char placeholder → 9-char id);
  "clean no-op" overstated (qualified to canonical-form); `line` semantics
  unpinned (added to INV-2 + a test); `n/a\b` notation aligned; insert path tied
  to `migrate_v2`'s `orphans[]`/`unclassified[]`. All verified fixes applied.

- **Loop 2** — **0C, 0H** (both lanes); the loop-1 High re-verified fixed
  (`target_not_found`/`target_ambiguous` now in `mcp-error-codes.md:71-72`; the
  `#`/`## ` vs `### ` resolution distinction confirmed stated correctly
  throughout). Residual was **completeness/precision only** — no
  structural/mechanical/architectural finding: refusal precedence didn't place
  `bad_args` in the order (added to INV-9); INV-1 omitted the
  `claudeintegration.cpp` schema `op` enum (added); `path_derived` had no
  INV/test (added); a `changed:false` no-op's commit/mtime behaviour (pinned:
  skips the write, INV-8); `bytes_delta`/`line` untested on a real write (added
  assertions); the insert path's lack of a finding-shaped gate (stated);
  duplicate-`ids` / `closure:null` edges (deduped / treated absent); `inserted`
  vs `changed` per-call semantics (§2.4). All applied.

- **Convergence (2026-07-05):** 2 loops. Loop 2 = 0C/0H with only
  completeness/precision residual → **polish-converged** (convergence = no
  structural/mechanical/architectural finding, not literal zero). Every
  load-bearing external claim re-verified live: the placeholder string is
  byte-exact against `renderFindingBlock`, the two refusal codes are registered +
  genuinely emitted by `compact_shipped`, the standard's §"Tooling" row is
  reconciled (`(new, ANTS-3447)`, `### ` enumerator, no `duplicate_target`), and
  the enumerator/insert/fence/idempotency claims match the code. Signed off as
  implementation-ready.
