# Feature test — ANTS-3396 opt-in async `audit_run` + `audit_poll`

Contract mirror of `docs/specs/ANTS-3396.md` in test-anchor form. Label
`features` (added to the `test_claude` bundle).

Wiring is verified by source-grep against `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and `src/claudeintegration.cpp`
(`SRC_CLAUDE_INTEGRATION_CPP_PATH`); the job-registry behaviour is
exercised directly against a `ClaudeIntegration` instance (the registry
methods are pure QHash/QMutex — no event loop needed).

| INV | Test | What it pins |
|-----|------|--------------|
| 1 | `Inv1SyncPathUnchanged` | scoped to the `audit_run` registration: no `wait()` anywhere in it, and a `QThread::finished` completion slot in each branch, so the synchronous branch replies later instead of joining (ANTS-2132 § 2.8, INV-16). The async branch is guarded by `args...("async").toBool()`. |
| 2 | `Inv2AsyncBranchNoJoin` | the async branch returns `{ok, async:true, job_id, status:"running", poll_with:"audit_poll"}` without a `wait()` between register and return. |
| 3 | `Inv3RegistryRunningThenDone` | `auditJobRegister` → poll `running`; `auditJobComplete` (done) → poll `done` with `cache_path` + counts; error → `status:"error"`; cache-write failure falls back to `r.sarifPath`. |
| 4 | `Inv4RegistryBoundAndExpiry` | ≥ 16 terminal jobs → size ≤ 16, oldest evicted, evicted id polls `expired` with a `last_audit_summary` hint; all-running saturation → `auditJobRegister` returns empty (→ `too_many_jobs`). |
| 5 | `Inv5QueuedCompletionAndGuardDismiss` | the completion uses `Qt::QueuedConnection`; the async branch calls `inFlightGuard.dismiss()`; the registry is mutated under `m_auditJobsMutex`. |
| 8 | `Inv8RefusalWiring` | `audit_poll` registered `Required` in `callerCwdContractFor`; `bad_args` on missing `job_id`; `too_many_jobs` literal on the async saturation path. |
| 9 | `Inv9NoNewConfigKey` | neither path reads a new `claude.*` config key. |
| 10 | `Inv10WorkerFreesItself` | (ANTS-5080) the async branch connects the worker's `QThread::finished` to its own `QObject::deleteLater`, and the completion slot whose context is `ClaudeIntegration` does not touch `worker`. Before this the worker was freed only inside that slot, so a `ClaudeIntegration` destroyed mid-sweep severed the connection and the thread was never freed. |

The full async→poll→done cycle against a real sweep is CI-gated (skipped
when the toolchain is absent) and is not part of this unconditional
source-grep + registry-bound coverage.
