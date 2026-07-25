# Feature spec: ANTS-1351 v1 — `audit_run` MCP verb

v1 ships the infrastructure (engine module, MCP verb, env scrub,
PathValidation, cap pipeline, in-flight gate). Rich AuditDialog
config-table integration + per-tool SARIF result extraction land
in v2 (roadmap follow-up).

## Invariants exercised by this test set

- **INV-1 / Aggregate cap constant.** `AuditRunner::kAggregateCapMs =
  900'000` (ANTS-3585 raised it 240'000 → 900'000). ANTS-3611 promoted it
  from an `auditrunner.cpp`-local constant to `auditrunner.h` so the MCP
  layer can derive from it; the test asserts the value, not a source byte
  pattern.
- **INV-2 / PathValidation on caller_cwd.** `canonicalFilePath` +
  `isDir` check before tool dispatch.
- **INV-3 / Required contract.** `audit_run ∈ Required` in
  `ClaudeIntegration::callerCwdContractFor`.
- **INV-5 / Per-tool cap pattern.** SIGTERM at cap, `kKillGraceMs`
  SIGKILL grace.
- **INV-9 / Inline in-flight gate.** `verbInFlightTryAcquire` /
  `verbInFlightRelease` on ClaudeIntegration.
- **INV-9b / Reap windows track the aggregate cap (ANTS-3611).**
  `kVerbInFlightReapMs` is `AuditRunner::kAggregateCapMs + 30'000` and
  `kAuditJobReapMs` is an alias of it — never a hardcoded literal. Before
  ANTS-3611 both were a stale `270'000` (240 s + 30 s slack) left behind
  when ANTS-3585 raised the cap to 900 s, so an audit legitimately running
  between 270 s and 900 s had its slot reaped as "worker death" and a
  second `audit_run` on the same root could start concurrently.
- **INV-10 / Env scrub.** Allowlist + blocklist + AWS_* prefix +
  LC_* prefix.
- **INV-10b / Absolute-path tool resolution.** Via
  `QStandardPaths::findExecutable` with TTL cache.
- **INV-13 / Sample-message cap + cascade.** 256 B cap;
  bottom-up cascade 10 → 5 → 3.
- **INV-15 / since-tag argv-safe.** Regex sanitiser
  `^[A-Za-z0-9._/+-]{1,128}$` + leading-`-` reject.
- **INV-16 / Range checks.** `cap_per_tool_seconds` [5, 300] (ANTS-3585
  raised the ceiling from 60); `top_findings_count` [0, 100].
- **INV-17 / Secret redaction of raw tool output (ANTS-2188).** Each
  tool's raw output is run through `SecretRedact::scrub` in the `finish`
  lambda *before* it is stored into `rawByTool` (→ the on-disk SARIF
  notification text) or handed to `parseToolOutput` (→ `samples` /
  `top_findings`). trivy runs `--scanners secret` and surfaces the
  literal secret value; gitleaks already runs `--redact`, but trivy did
  not, so an unredacted secret would otherwise reach both the
  `.audit_cache/*.sarif` artifact and the MCP envelope returned to the
  LLM (OWASP LLM06). One scrub at the single capture point covers both
  sinks; the scrubbed value (not the raw) must feed `rawByTool` and
  `parseToolOutput`.
- **INV-18 / Scoped-positional argv-injection guard (ANTS-2185).** A
  scoped positional path that begins with `-` (a file named e.g.
  `-rf.cpp` in a hostile-clone tree) would be parsed as a flag by the
  child tool. `toolArgv` normalises every scoped path through
  `flagSafeScopedPathImpl`, which prefixes `./` to dash-leading relative
  paths so they are unambiguously paths; absolute and ordinary relative
  paths pass through byte-identical (so the since-last-run delta keeps
  matching them). No tool branch may append the raw `scopedPaths`. A
  `--` end-of-options separator is *not* used: ruff/bandit/shellcheck/
  mypy append flags *after* the path list, which `--` would swallow.
- **INV-19 / Compile-DB resolution for the C/C++ tools (ANTS-2182).**
  `AuditEngine::resolveCompileCommands(projectRoot)` probes the canonical
  build-dir variants (`build`, `build-fast`, `build-asan`,
  `build-workstation`, `build-release`, `build-debug`, `build-test`) and
  returns the first `<dir>/compile_commands.json` that exists, else an
  empty string. The MCP `toolArgv` path uses it so cppcheck/clazy/
  clang-tidy resolve Qt system headers + per-TU compile flags from the
  build DB instead of flooding `missingIncludeSystem` / mis-parsing
  `namespace X {` as invalid C (cppcheck) or returning 0 findings (clazy
  when the DB lives in a non-`build/` tree). cppcheck gets `--project=<db>`
  when a DB is found (with the missing-include suppressions as a
  belt-and-suspenders / no-DB fallback); clazy and clang-tidy resolve
  their `-p` argument through the same helper rather than a hardcoded
  `build/` path.
- **INV-20 / `audit_run` transport-cap guidance (ANTS-2183).** The
  `audit_run` verb description documents that a full-tree sweep can exceed
  the MCP client's ~60 s request timer (Claude Code's), surfacing
  `transport: timed out` *outside* the response even though the run
  completes server-side and writes its SARIF — and that the result is read
  back via `last_audit_summary` (or the envelope's `cache_path`). Mirrors
  the two-tier-timeout note `verify_changes` already carries.
- **INV-21 / in-process drift lanes dispatched headless (ANTS-3605).**
  A default auto-detect sweep (`tools:[]`) at full scope runs the GUI-free
  `FeatureCoverage` lanes — `spec_code_drift`, `contract_doc_drift`,
  `changelog_test_coverage` — inside `runAudit`, after the external
  QProcess tools and before the tally, feeding each through the same
  `finish()` seam so they land in `by_tool[<lane-id>]`. An explicit
  `tools:[…]` request or a narrowed file-diff scope skips them (whole-
  project checks). Relaxes INV-14: an empty-`tools` sweep with no external
  tool on PATH no longer refuses `no_tools_runnable` — it proceeds so the
  lanes (which need no external binary) still run, matching the GUI dialog.
  Test: `Ants3605InProcessLanesDispatchedHeadless` — a hermetic
  QTemporaryDir project whose `tests/features/*/spec.md` cites a
  back-ticked token absent from the tree; asserts `spec_code_drift` lands
  in `by_tool` with `rawCount ≥ 1`. See docs/specs/ANTS-1351.md INV-21.
