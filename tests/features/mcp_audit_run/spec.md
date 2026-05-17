# Feature spec: ANTS-1351 v1 — `audit_run` MCP verb

v1 ships the infrastructure (engine module, MCP verb, env scrub,
PathValidation, cap pipeline, in-flight gate). Rich AuditDialog
config-table integration + per-tool SARIF result extraction land
in v2 (roadmap follow-up).

## Invariants exercised by this test set

- **INV-1 / Aggregate cap constant.** `kAggregateCapMs = 240'000`
  defined in auditrunner.cpp.
- **INV-2 / PathValidation on caller_cwd.** `canonicalFilePath` +
  `isDir` check before tool dispatch.
- **INV-3 / Required contract.** `audit_run ∈ Required` in
  `ClaudeIntegration::callerCwdContractFor`.
- **INV-5 / Per-tool cap pattern.** SIGTERM at cap, `kKillGraceMs`
  SIGKILL grace.
- **INV-9 / Inline in-flight gate.** `verbInFlightTryAcquire` /
  `verbInFlightRelease` on ClaudeIntegration.
- **INV-10 / Env scrub.** Allowlist + blocklist + AWS_* prefix +
  LC_* prefix.
- **INV-10b / Absolute-path tool resolution.** Via
  `QStandardPaths::findExecutable` with TTL cache.
- **INV-13 / Sample-message cap + cascade.** 256 B cap;
  bottom-up cascade 10 → 5 → 3.
- **INV-15 / since-tag argv-safe.** Regex sanitiser
  `^[A-Za-z0-9._/+-]{1,128}$` + leading-`-` reject.
- **INV-16 / Range checks.** `cap_per_tool_seconds` [5, 60];
  `top_findings_count` [0, 100].
