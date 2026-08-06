# mcp_last_audit_summary_since_commit — ANTS-1406

Locks the wiring contract for the `since_commit` short-circuit
parameter on `last_audit_summary`. /close-phase dispatches /audit
+ /indie-review in parallel; on clean closes /audit returns "all
gates green + 0 actionable" after burning ~45-50 K tokens
re-running the same gates already known green at HEAD. This
parameter lets the caller ask "is there an audit-clean snapshot
already cached at HEAD?" and skip the second pass when yes.

## Contract

When `since_commit` is set:

1. Server compares the supplied SHA against the cached summary's
   `commit` field (filled by SARIF provenance or read-time
   `git rev-parse HEAD` fallback).
2. Case-insensitive hex compare; the shorter of the two strings
   is treated as a prefix (≥ 7 hex chars required to count as a
   commit match).
3. AND the report's mtime is within a 5-minute freshness window.
4. Both gates pass → return the full summary envelope plus
   `fresh:true`.
5. Otherwise → short envelope:
   `{ok:true, fresh:false, since_commit, last_run_commit?,
   last_run_age_ms, reason:"commit_drift" | "stale_mtime" |
   "no_provenance"}`.

## Invariants

| # | Statement |
|---|-----------|
| 1 | `last_audit_summary` inputSchema in `src/claudeintegration.cpp` declares a `since_commit` property of `type:"string"`. |
| 2 | The property description mentions ANTS-1406 and the `fresh` response field. |
| 3 | The tool top-level description mentions `since_commit:<sha>` and `fresh:false` so the assistant can discover the contract without inspecting the schema. |
| 4 | `cmdLastAuditSummary` in the remotecontrol TUs declares a `kSinceCommitFreshnessMs` constant equal to `5 * 60 * 1000` (5-minute window). |
| 5 | The implementation branches on three distinct reason codes — `commit_drift`, `stale_mtime`, `no_provenance` — at least one literal match per code. |
| 6 | The short envelope includes the `last_run_age_ms` field name (callers depend on it for staleness telemetry). |
| 7 | The fresh-path response carries a `fresh:true` flag when `since_commit` was passed (callers depend on its presence to confirm the gate landed). |
| 8 | The minimum match length is enforced at 7 hex characters (`n >= 7` in source). |

## Acceptance

Exit 0 = all 8 invariants hold. Pure source-grep harness — no
runtime fixture (the existing `mcp_last_audit_summary` test
already exercises the parse path; this one locks only the new
short-circuit gate's wiring).

## Out of scope

- Live cmdLastAuditSummary invocation (would require a synthetic
  `.audit_cache/` + provenance SARIF; not worth the boilerplate
  for a contract this narrow).
- `audit_precondition_summary` v2 — option (b) from the ANTS-1406
  bullet, separate tool that reports gate state without re-running
  individual scanners. Deferred until /close-phase user feedback
  flags option (a) as insufficient.
