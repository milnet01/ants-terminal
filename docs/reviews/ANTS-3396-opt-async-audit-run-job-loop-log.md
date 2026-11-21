# ANTS-3396 — opt-in async `audit_run` (job handle + `audit_poll`) — review loop log

Record for [`../specs/ANTS-3396-opt-async-audit-run-job.md`](../specs/ANTS-3396-opt-async-audit-run-job.md) § 5. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-07-01)** — 2 independent cold reviewers. Verified &
  folded: 1 CRITICAL (cache-write-failure leaves `cache_path` empty →
  `done` now falls back to `r.sarifPath`, handles `noChanges`),
  broken cross-refs (fabricated `ANTS-1648` → real `ANTS-1569`;
  `ANTS-2183` relabelled as a ROADMAP bullet, not a spec file), master
  `claude.mcp_enabled` gate + `bad_args`/`too_many_jobs` refusal wiring
  for `audit_poll`/`async`, the `qScopeGuard` double-release hazard on
  the async branch (must dismiss the guard), `last_audit_summary`
  returns the *newest* SARIF not the job's, `job_id` counter
  process-lifetime/reset, all-running cap saturation, `expired` is a
  terminal `ok:true` status, `done`-key→`RunResult` field-mapping table,
  and several LOWs (single-line `note`, soften the SARIF-size figure,
  `elapsed_ms`-omitted-on-`done`, `expired` is poll-synthesised). All
  code citations re-verified against current source.
- **Loop 2 (2026-07-01)** — 2 independent cold reviewers, cold on the
  edited draft. Zero CRITICAL/HIGH accuracy findings — all loop-1 fixes
  held and every code citation re-verified exact (`RunResult`
  mapping-table line numbers, the `mainwindow.cpp` join/guard cites, the
  `verbInFlight`/`callerCwdContractFor` cites, the broken-cross-ref
  fixes). Folded: 1 HIGH (the loop-1 guard fix offered an unsafe "or
  branch before the guard is armed" alternative — now mandates
  `inFlightGuard.dismiss()` as the single contract + drop `const`),
  2 MEDIUM (`audit_poll` `code` is job-scoped, so branch on `status` not
  `ok`; `too_many_jobs` `retry_after_ms` pinned to 5000), plus LOW /
  clarity (Status-header currency, `canon` cite to :4511/:4504, soften
  INV-1 "byte-identical", mutex-is-defensive rationale, shutdown
  worker-teardown safety, eviction-by-`startedMs`, idempotent re-poll).
  One Medium (a `cmdLastAuditSummary` line drift) was non-actionable —
  the spec cites it by symbol name, not line.
- **Loop 3 (2026-07-01)** — 2 independent cold reviewers, both cold.
  Zero CRITICAL; both converged on one finding: the loop-2 shutdown
  paragraph over-promised an unanchored "join at shutdown" that
  contradicted the no-`wait()` premise and was untested. Folded:
  rewrote shutdown as an explicit open implementation decision (mandate
  only the `QPointer`-guarded completion slot; no full join; no data at
  risk since the SARIF is durable) with no INV/Test; sharpened the
  `last_audit_summary` mechanism from "recency heuristic" to
  "lexicographically-greatest `audit-<iso-utc>-<sha>` basename"
  (`remotecontrol.cpp:11804-11813`); added the empty-`r.code` fallback
  (omit `code`, rely on `error`); stated the worker is created on the
  main thread (so `deleteLater()`/ownership are same-thread); made the
  start-time eviction ordering a stated deliberate simplification. Every
  code citation re-verified exact a third time. The fixes are confined
  to text loops 1–2 introduced.
- **Loop 4 (2026-07-01)** — 2 independent cold reviewers, both cold.
  **Clean: zero CRITICAL / HIGH / MEDIUM**; both verdicts
  "implementation-ready". All loop-3 fixes held — the rewritten shutdown
  paragraph no longer contradicts the no-`wait()` premise, the lex-max
  wording matches code, the empty-`code` fallback and worker-thread
  affinity are accurate; every code citation re-verified exact a fourth
  time. Folded the only residual nits: dropped the drift-prone line-number
  column from the field-mapping table (symbol-anchored only), numbered
  the eviction order (reap → evict-terminal → `too_many_jobs`), and noted
  the behavioural async→poll cycle test is CI-gated like the refusal-path
  tests. **Converged.**
- **Loop 5 (2026-07-01, implementation).** Implemented against the
  converged spec. One refinement surfaced by the ANTS-1642
  forward-completeness lint (every declared schema prop must be read by
  its lambda): `audit_poll` declared `caller_cwd` but the first draft did
  not read it, because the spec keyed the poll on `job_id` alone. Rather
  than satisfy the lint with a dead `args.value("caller_cwd")`, the poll
  now resolves `caller_cwd` to the canonical root and scopes the registry
  read to it (cross-root → `expired`) — the process-global registry made
  an unscoped poll a (theoretical, single-project host) cross-project
  `cache_path` leak, and root-anchoring is the established Required-verb
  posture (INV-10 added). Also folded during implementation: the
  `kindForName` catalog bucket for `audit_poll` (audit family) and the
  `async`/`audit_poll` schema descriptors. Full suite green, 2402/2402.
