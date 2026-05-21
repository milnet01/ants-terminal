# Ants Terminal

Qt6/C++20 terminal emulator. Optional Lua 5.4 plugins. `libutil` for PTY.
CMake build.

## Module map (src/)

Listed only where behavior isn't obvious from the name.

- `vtparser` — VT100/xterm state machine. Emits `VtAction` structs
  (Print/Execute/CSI/ESC/OSC/DCS/APC).
- `terminalgrid` — cell grid + scrollback + cursor + modes. Owns:
  OSC 8 hyperlinks, OSC 52 clipboard, OSC 133 shell integration,
  OSC 9/777 notifications, OSC 9;4 progress (disambiguated from OSC 9
  by first-byte `4;` peek), Kitty keyboard protocol (push/pop stack),
  Sixel/Kitty-APC/iTerm2 images, DA/CPR/DSR response callback,
  per-line `combining` side-table for zero-cost non-combining lines,
  soft-wrap reflow on resize.
- `terminalwidget` (QWidget — was QOpenGLWidget pre-0.7.4; the dormant
  glyph-atlas `GlRenderer` was retired in 0.7.44) — QPainter +
  QTextLayout renderer with HarfBuzz ligatures. SGR mouse, focus
  reporting, sync output, undercurl, per-pixel bg alpha.
- `ptyhandler` — forkpty + QSocketNotifier.
- `auditdialog` — static-analysis panel. Pipeline (matches
  `handleCheckOutput` order):
  `OutputFilter → parseFindings → mark .audit_suppress → drop
   generated-file → drop/shift path_rules → drop allowlist →
   drop inline-suppress → drop non-recent → dedup →
   comment/string filter → mypy-stub fold → cap →
   enrichment (snippet ±3, git blame, confidence 0-100) →
   trend → render + SARIF v2.1.0 / HTML`.
  Rationale: `.audit_suppress` *marks* `f.suppressed=true` so SARIF
  `result.suppressions[]` § 3.34 surfaces them (doesn't drop). Dedup
  runs after drop steps to avoid double-counting on findings that
  would have been dropped anyway. Comment/string + mypy-fold run
  before cap so junk doesn't push real findings out.
  Recognizes foreign suppression markers (NOLINT, cppcheck-suppress,
  noqa, nosec, nosemgrep, #gitleaks:allow, eslint-disable-*,
  pylint: disable) plus native `// ants-audit: disable[=rule]`.
- `auditengine` (Qt6::Core only) — pure-function counterparts of the
  dialog's parsing pipeline (`applyFilter`, `parseFindings`,
  `capFindings`, `sourceForCheck`, `computeDedup`,
  `isCatastrophicRegex`, `hardenUserRegex`). Non-GUI consumers
  (CI runners, ants-helper v2 audit-run, future MCP) link this
  without dragging Qt6::Widgets in. ANTS-1119.
- `auditcache` (Qt6::Core only) — per-project `.audit_cache/`
  infrastructure for `audit_run` (ANTS-1555). Routes SARIF + a
  `.audit_cache/index.json` manifest (v1, `{last_run, history[]}`)
  into `<root>/.audit_cache/audit-<iso-utc>-<sha>.sarif`, the same
  dir `auditdialog`'s Export-SARIF button writes to. Atomic via
  `QSaveFile` + 0600 via `setOwnerOnlyPerms`. Retention reaper
  caps history at 10 entries; deletes only sarif/html files named
  in dropped manifest entries (never AuditDialog GUI artefacts,
  `trend.json`, or `baseline.json`). `AuditRunner::RunResult`
  gains `cachePath` (sarif path under `.audit_cache/`; empty on
  read-only roots → `/tmp` fallback via `allocSarifPath`) +
  `priorRun` (manifest's pre-existing `last_run` snapshot) — the
  anchor for ANTS-1504 since-last-run mode. Spec
  `docs/specs/ANTS-1555.md`.
- `audithygiene` — splices project-local scanner config into invocations
  (`.semgrep.yml` header → `--exclude-rule`; `pyproject.toml` ruff S-codes
  → bandit `--skip B<nnn>`).
- `auditautofix` (Qt6::Core only; in `ants_audit_lib`) — native safe-list
  auto-fixer for audit findings (ANTS-1719). `ants::autofix::planRepair`
  returns a single-line behaviour-neutral `Repair` only when a finding is
  provably one of: dead `#include` (cppcheck `unusedInclude`), standalone
  dead `Q_UNUSED(...)`, expired `// … TODO … remove after X.Y.Z`, or
  `//word`→`// word`; each gated on BOTH a finding signal and the exact
  line shape. `applyRepair` writes atomically (`QSaveFile`) and re-checks
  the line against `Repair.original` first (refuses a stale plan).
  `logRepair` appends to `.audit_cache/autofix-*.jsonl`. Driven by the
  opt-in "Auto-fix safe" `AuditDialog` button — never on a scan.
- `falseposledger` (Qt6::Core only; in `ants_core_lib`) — load + filter
  + format helpers for `.ants_review_falsepos.jsonl`, the prose-grain
  false-positive ledger shared across the three AI-reviewer sweep
  skills (`/cold-eyes`, `/indie-review`, `/test-audit`). Read-only
  in v1; CC sessions append via `printf '\n%s\n' "$record" >> …` per
  the standard at `docs/standards/audit-false-positives.md`. The
  three engines' brief-assembly paths call into this module to
  inject a "previously-rejected findings" block (text form) or
  `prior_false_positives` array (test-audit). Fence-hardening
  mirrors ANTS-1352. Distinct from line-grain `.audit_suppress`
  which `auditdialog` owns for static-analyser findings. ANTS-1457.
- `auditfpledger` (Qt6::Core only; in `ants_core_lib`) — fingerprint-
  keyed learned-FP ledger for *static-audit* findings (the code-grain
  sibling of `falseposledger`'s prose-grain ledger). `computeFingerprint`
  is line-INDEPENDENT (strips a `<path>:<line>[:<col>]:` prefix before
  hashing file+checkId+message → 16 hex), so a learned FP survives the
  line shifts that break the `dedupKey`-grain `.audit_suppress`. Loads /
  filters / atomic-appends `<root>/.audit_cache/learned-fp.jsonl`.
  `AuditEngine::applyLearnedFpSuppressions` consumes the fingerprint set
  to MARK findings `suppressed` (SARIF `suppressions[]`, not dropped) —
  shared by `auditdialog` and the headless `audit_run` path (ANTS-1706).
  The dialog suppress action records to BOTH this ledger and
  `.audit_suppress`. `audit_dismiss` MCP verb deferred to ANTS-1713.
  ANTS-1708.
- `featurecoverage` — in-process audit lanes via `AuditCheck::inProcessRunner`
  (no QProcess). Two in-process: `spec_code_drift`,
  `changelog_test_coverage`. Plus `test_health`, which is implemented
  shell-side (`auditdialog.cpp` recursive grep) — listed alongside for
  topical grouping; not routed through `inProcessRunner`.
- `focusedtest` (Qt6::Core only) — pure resolution for the
  `focused_test` MCP tool (ANTS-1302): maps changed files →
  `ctest -R` patterns via `tests/coverage-map.json` (schema v1:
  `map` src→patterns + optional `default`/`ignore`), a basename
  heuristic, or a conservative full-suite fallback (an unmapped
  source file or a 0-match selection always falls back to full).
  `cmdFocusedTest` (in `remotecontrol.cpp`) runs the ctest subset and
  parses via `TestResCache::parseCtestOutput`. `tests/coverage-map.json`
  is the project's living, partial file→test map. Spec
  `docs/specs/ANTS-1302.md`.
- `mcpprojection` (Qt6::Core only) — pure `fields=` response projection
  for high-volume MCP read tools (ANTS-1720). `mcp::projectFields`
  narrows a JSON-object response to named top-level fields;
  `mcp::isFieldProjectionTool` is the 7-tool allowlist (a subset of
  `isEtagSupportedTool`). Called from the `tools/call` dispatch after
  `applyEtagPattern`, before `wrapMcpData`.
- `briefdispatch` (Qt6::Core only; `ants_core_lib`) — shared dispatch-brief
  composer for the review-dialog family (ANTS-1727). `BriefDispatch::fenceBody`
  is the 4-backtick fence-hardening kernel extracted from
  `IndieReviewEngine::assembleBriefForDispatch` (which now calls it; the
  standards-doc loop passes a `"standard"` label so its brief stays
  byte-identical). `inlineBodies` fences a path list; `inlineRelevantSections`
  slices large cross-reference docs (e.g. ROADMAP.md) to only the `##`/`###`
  sections matching a keyword, with an H1+intro leading-block fallback.
- `llmclient` (Qt6::Core + Qt6::Network, widget-free; `ants_core_lib`) —
  reusable OpenAI-compatible streaming chat client extracted from `AiDialog`
  (ANTS-1727). Owns the request build, SSE drain (256-line/tick + re-arm),
  10 MiB caps, scheme allowlist, transfer timeout, and OWASP LLM06 scrub.
  Static test seams `isEndpointAllowed`/`isPlaintextRemote`/`sseContentDelta`
  /`buildRequestBody`/`accumulateCapped` make the parse/scrub/cap logic
  unit-testable without a network. `AiDialog` now drives this for the
  network; it keeps its own scrub + redaction notice (UX-coupled) and passes
  prompts pre-scrubbed (`scrubSecrets=false`).
- `llmdispatcher` (Qt6::Core + Qt6::Network, widget-free; `ants_core_lib`) —
  bounded-concurrency pool over `LlmClient` (ANTS-1727). `JobRunner` test
  seam; `maxConcurrent` clamped `[1,4]` (config `ai_review_concurrency`,
  default 2). Emits `jobFinished` per job + `allFinished`; retains no result
  text after forwarding (RAM bound). `cancelAll` aborts in-flight + drains.
- `reviewdialogbase` (`ants_dialogs_lib`) — shared QDialog scaffold for the
  v2 review-dialog family (ANTS-1721 ColdEyes / ANTS-1722 TestAudit, plus the
  deferred audit-v2 / indie-review dialogs). Owns partition tabs, the
  Dispatch button (→ `LlmDispatcher` at `ai_review_concurrency`), a results
  host, and the Fold-into-ROADMAP button. Hooks: `derivePartition` /
  `composeBrief` / `onAllReportsCollected` / `performFoldIn` (subclass owns
  the fold-in sequence — base provides `activeReleaseHeading` /
  `allocateFoldInIds` / `insertFoldInBlock` helpers, not an orchestrator, so
  test-audit's engine-owned fold-in doesn't double-allocate). `dispatchOne`
  runs a follow-up (e.g. synthesis) without re-entering
  `onAllReportsCollected`. Static `endpointDispatchable` gates Dispatch.
- `remotecontrol` — Kitty-style JSON-over-Unix-socket IPC. Verbs:
  `ls`, `send-text`, `new-tab`, `select-window`, `set-title`,
  `get-text`, `launch`, `tab-list`, `roadmap-query`,
  `workspace-search`, `file-outline`, `find-definition`,
  `find-caller` (ANTS-1303), `similar-code` (ANTS-1305),
  `git-state`, `subsystem`, `roadmap-branch-drift` (ANTS-1583).
  Trust model: UID-scoped + 0700 perms + `lstat`-checked
  `S_ISSOCK`.
- `antshelper` (optional CLI, `-DANTS_ENABLE_HELPER_CLI=ON`) — local
  subagent for Claude Code; v1 surface is `drift-check`. ANTS-1116.
- `luaengine` / `pluginmanager` — sandboxed Lua 5.4; plugins live in
  `~/.config/ants-terminal/plugins/`, gated by `ANTS_LUA_PLUGINS`.
- `claudeintegration` — singleton owning the Claude Code hook
  server (one UDS shared across every Claude under any tab),
  `m_pollTimer` (2 s) for `pollClaudeProcess` PID detection,
  `sessionPathForCwd` (project-scoped JSONL resolver with
  process-start anchor + 24 h / 5 min liveness floor — ANTS-1163),
  `processHookEvent` with the `isFocusedTabSession` gate (ANTS-1161
  drops foreign-tab hook events from mutating singleton state).
  Per-tab transcript binding via `m_transcriptPath`; PermissionRequest
  routes through `m_lastHookSessionId` rather than the gate.
- `claudestatuswidgets` — `ClaudeStatusBarController` owns the
  bottom-bar Claude chips (review-changes, audit, bg-tasks, tasks,
  context-bar, error, repo, model-recommender). `refreshTasksButton` /
  `refreshBgTasksButton` / `refreshModelChip` fire on the 2 s status
  timer, call `activeSessionPath(focusedCwd)`, push the path to
  `m_tasks` / `m_bgTasks` only on change, and call `poll()` /
  `sweepLiveness()` for atomic-rewrite watch-loss recovery.
  `refreshModelChip` calls `ModelRecommender::score(transcriptPath)`
  and shows/hides `m_modelBtn` based on whether the recommended tier
  differs from `message.model` in the most-recent transcript turn.
  `resetForTabSwitch` clears trackers synchronously on tab change.
- `modelrecommender` (Qt6::Core only) — stateless free-function scorer
  `ModelRecommender::score(transcriptPath)` that tail-reads ≤ 512 KB
  from the active session's JSONL, scores the last 20 assistant turns
  for file-write count, tool diversity, plan keywords, and message
  length, and returns a `Tier` (Haiku/Sonnet/Opus) with a reason
  string and the current model ID from `message.model`. Used by
  `claudestatuswidgets` for the ANTS-1226 model recommender chip.
  No side effects; safe to call on the UI thread. ANTS-1226.
- `roadmapdialog` — ROADMAP.md viewer. Two renderers coexist:
  `renderHtml` (the v1 markdown→HTML helper, kept for tests + the
  `roadmap-query` IPC verb consumers) and `renderCardsHtml` (the v2
  card-style renderer the dialog now uses). v2 wraps each
  status-emoji bullet as a `<div class="rm-card">` with state icon +
  Kind chip + summary + meta row; section headers (`##`/`###`) emit
  collapse anchors using the `ants://expand-section/<slug>` URL
  scheme handled by `handleAnchorClicked`. Per-item / per-section
  expand state persists via four `Config::roadmap*` keys.
  `parseShippedDates` resolves `[ANTS-NNNN]` → CHANGELOG release
  date for ✅ cards. Tab-relevance gating drops prose narration on
  non-Full presets (INV-11/12). Spec: `docs/specs/ANTS-1154.md`.
- `claudetasklist` / `claudebgtasks` — per-tab JSONL trackers with
  `QFileSystemWatcher` + `poll()` / `sweepLiveness()` mtime rescue
  for the case Claude rewrites the transcript via tmpfile+rename
  (which silently drops the watch). **Both** trackers now expose a
  `poll()` that rebinds the watch when the file appears or its
  mtime advanced; bg-tasks gained parity in 2026-05-13. Parser:
  `TodoWrite` (snapshot replace), `TaskCreate` + paired tool_result
  (incremental add), `TaskUpdate` (status flip). The chip itself
  reads `done/total` (ANTS-1246); `unfinishedCount() = pending
  only` (ANTS-1221) is retained as a diagnostic accessor.
  `claudebgtasks::parseTranscript` filters `isSidechain`
  (subagent inline turns) and resets state on `isCompactSummary`,
  matching the foreground tracker.

## Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

## Build & test

```bash
cmake -G Ninja -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Tests are built by default (`ANTS_TESTS=ON` since ANTS-1217 Phase 7,
2026-05-10). Pass `-DANTS_TESTS=OFF` for a fast main-exe-only iteration.

Use Ninja, not Make: the `JOB_POOLS` cap in `CMakeLists.txt`
(`compile_pool=max(2, nproc/4)` + `link_pool=1`) only applies under
Ninja. Make ignores the pool and runs whatever `-j` you give it,
which on a workstation with several Qt-bloated cc1plus jobs in flight
is the path that earlyoom-reaped binaries in 0.7.x.

**Token-frugal build invocations** (for AI assistants reading the
output):

```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

This keeps a 10k-line build log out of the assistant's context window
while preserving the tail where compile/link failures actually surface.

### Cheaper iteration loops

Five overlapping knobs trim wall-clock and peak RAM on the workstation
edit-build-test loop (ANTS-1550 / ANTS-1552):

- `cmake --build build --target ants-terminal` — skips the ~30 test
  bundle binaries when only the main app changed.
- `-DANTS_TESTS=OFF` at configure time — drops every test target from
  the graph entirely. Pair with the target-specific build above for
  the absolute floor.
- `-DANTS_CCACHE=ON` (default) — wires `ccache` as the C/C++ compiler
  launcher when installed; on a cache hit `cc1plus` is not invoked
  at all (zero RSS, zero time). Pass `-DANTS_CCACHE=OFF` to disable
  if the auto-pick is unwanted.
- `-DANTS_UNITY_BUILD=ON` — folds groups of `.cpp` into one TU per
  CMake target, dropping concurrent `cc1plus` instance count. **Opt-
  in, experimental.** The current STATIC-lib layout exposes two
  cross-cutting issues: (a) per-test-bundle `static int runMain()`
  helpers collide when unified (test bundles are exempted via
  `UNITY_BUILD OFF`); (b) merging TUs inside `ants_core_lib`
  produces unity `.o` files that reference symbols from higher libs
  (`MainWindow`, `RoadmapDialog`), which breaks the test bundles'
  selective `--start-group` link. ANTS-1553 tracks the lib-boundary
  rework needed to make Unity work end-to-end.
- `cmake --preset=fast` — bundles ccache + per-lib PCH in
  `build-fast/`. Recommended starting point for hot-loop work.
  Unity is intentionally *not* enabled in the preset until
  ANTS-1553 ships.

Layer 3 backstop (`tools/safe-build.sh`, below) still applies if a
future regression reintroduces over-parallelism — the new knobs lower
the working-set ceiling, not the safety net.

### CMake presets

`CMakePresets.json` ships four presets:

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`. Honours the in-tree JOB_POOLS cap. |
| `workstation` | Release in `build-workstation/` hard-capped at `-j3` for constrained hardware *or* when the build is competing with a heavy desktop session. Pair with `cmake --build --preset=workstation`. |
| `debug` | Debug + ASan/UBSan in `build-asan/` with sanitizer env vars wired into `ctest --preset=debug`. |
| `fast` | Release in `build-fast/` bundling ccache + Unity + per-lib PCH (ANTS-1550). Lowest wall-clock for incremental hot-loop work on a warm cache. |

```bash
cmake --preset=default    && cmake --build --preset=default    && ctest --preset=default
cmake --preset=workstation && cmake --build --preset=workstation && ctest --preset=workstation
cmake --preset=debug      && cmake --build --preset=debug      && ctest --preset=debug
cmake --preset=fast       && cmake --build --preset=fast       && ctest --preset=fast
```

### Last-resort backstop: `tools/safe-build.sh`

Layer 3 of the ANTS-1217 build-OOM guardrails. Wraps `cmake --build`
in a systemd-user scope with `MemoryMax=24G` + `MemorySwapMax=8G`, so
if a future regression ever reintroduces the cc1plus over-parallelism
that triggered the 2026-05-09 incident, the kernel kills the *build*
instead of the active session. Layer 1 (in-tree JOB_POOLS) and Layer 2
(`workstation` preset) should make this unnecessary; reach for it only
after kernel updates or new Qt major releases.

```bash
tools/safe-build.sh                 # builds ./build with defaults
tools/safe-build.sh build-asan      # builds ./build-asan
tools/safe-build.sh build -j$(nproc)  # passes extra flags through
```

Optional audit deps probe with `which <tool>` and self-disable if
absent (clazy, semgrep, osv-scanner, trufflehog, hadolint, checkov,
ast-grep, cppcheck, clang-tidy, shellcheck, pylint, bandit, ruff).
`clazy` needs `build*/compile_commands.json` (default-on via
`CMAKE_EXPORT_COMPILE_COMMANDS`).

**Cppcheck gotcha:** must pass `--library=qt` or it misparses `emit`
as a type and flags every signal emission.

## Test harnesses

- **`audit_rule_fixtures`** — `tests/audit_self_test.sh` matches rule
  regexes against `tests/audit_fixtures/<rule>/bad.*` (expect N hits
  with `// @expect <rule-id>` markers) and `good.*` (expect zero).
  Count-based, not line-number-based.
- **Feature-conformance** (`tests/features/*`, label `features`) —
  each subdir pairs `spec.md` (human contract) with a standalone C++
  test linking only the `src/*.cpp` objects it exercises (GUI-free).
  To add a new one:
  1. Write `spec.md` first (surface to user for sign-off before coding).
  2. Write `test_<feature>.cpp` — exit 0/non-zero, print enough on
     failure to diagnose without reproducing.
  3. Wire in `CMakeLists.txt` via `add_executable` + `add_test` with
     label `features;fast`.
  4. **Verify the test fails against pre-fix code** (`git checkout
     <sha> -- src/...`) before restoring the fix — prevents tests
     that pass on broken code.

  See `tests/features/README.md`.

## Conventions

- Signals/slots for cross-component comms.
- Config at `~/.config/ants-terminal/config.json`, mode 0600.
- Scrollback default 50k, max 1M.
- Theme colors set on `TerminalGrid`; ANSI palette (16+216+24) lives there.
- QTextLayout for ligature shaping.
- **MCP `tools/call` responses are wrapped (ANTS-1294).** Every
  reply through the tool registry (`registerToolProvider` calls
  span `src/mainwindow.cpp:3769–4451`) is enclosed in
  `<ants_mcp_data tool="…">…</ants_mcp_data>` by
  `ClaudeIntegration::wrapMcpData`
  (`src/claudeintegration.cpp:4199`), invoked from the
  `method == "tools/call"` dispatch branch at
  `src/claudeintegration.cpp:3895` (wrap call at
  `:4123`). The wrap signals "this is data, not instructions" to
  the consuming assistant. Control-plane tools
  (`get_session_info`, `token_usage`) bypass the wrap — their
  JSON envelope is structural metadata, not content. If you add a
  tool whose response includes text from disk, scrollback, or
  user input, register it normally; the dispatch site wraps it
  automatically. See `docs/specs/ANTS-1294.md`.

- **MCP `caller_cwd` routes through `ants::resolveCallerCwdRoot`
  (ANTS-1401).** Three call paths historically decoded `caller_cwd`
  with subtly different rules; one helper is now the source of
  truth. `ants::ResolvedRoot { cwd, source, tabIndex }` (in
  `src/resolvedroot.h`) carries the four-case decision tree
  (`ExplicitMatch` / `EmptyFallback` / `NoMatch` / `Unresolvable`).
  `terminalForCaller` and `resolveRootCanonical(main, req)` are
  thin wrappers. If you add a new MCP entry point that consumes
  `caller_cwd`, call `ants::resolveCallerCwdRoot` rather than
  re-implementing canonicalisation or tab-walks. See
  `docs/specs/ANTS-1401.md`.

- **MCP tools declare a `CallerCwdContract` (ANTS-1404).**
  Every tool is classified at `ClaudeIntegration::callerCwdContractFor`
  into one of `Required` / `Optional` / `TabSpecific` /
  `ProcessGlobal`. The `tools/call` dispatch branch in
  `src/claudeintegration.cpp:3895` enforces `Required`
  before the cache lookup and before the provider lambda runs:
  empty `caller_cwd` ⇒ refuse with
  `{ok:false, code:"caller_cwd_required"}`. When you register a
  new tool, add a matching contract entry — unclassified tools
  default to `Optional` (no enforcement), which is safe but loses
  the explicit declaration. `TabSpecific` is classified but not
  enforced in Phase 3a. See `docs/specs/ANTS-1404.md`.

- **MCP read tools opt into the ETag "304 Not Modified" pattern
  (ANTS-1499).** Thirteen tools (`project_layout`, `roadmap_query`,
  `file_outline`, `last_audit_summary`, `get_environment`,
  `tab_list`, `subsystem`, `git_state`, `roadmap_branch_drift`,
  `current_state`, `build_status`, `test_results`, `session_brief`)
  emit an `etag` field
  (sha256-hex16 of the canonical response body) and accept an
  `etag_match` input. When the caller passes a matching etag,
  `ClaudeIntegration::applyEtagPattern` short-circuits to
  `{ok:true, unchanged:true, etag:"<same>"}` instead of re-emitting
  the full body. Hooked at the dispatch site after the idempotent-
  read cache (so cache stores the un-etagged form) and before the
  `<ants_mcp_data>` wrap (so the hash covers the JSON envelope, not
  the wrapper tag). Allowlist is in `isEtagSupportedTool`; add a
  tool there + a `makeEtagMatchProp()` line to its schema to
  opt-in. Non-JSON responses are returned unmodified.

- **MCP read tools opt into `fields=` response projection
  (ANTS-1720).** Seven high-volume read tools (`roadmap_query`,
  `project_layout`, `file_outline`, `get_environment`, `tab_list`,
  `subsystem`, `git_state`) accept an optional `fields:["f1","f2"]`
  array that narrows the response to the named top-level fields.
  Pure logic is `mcp::projectFields` + `mcp::isFieldProjectionTool`
  in `src/mcpprojection.cpp` (Qt6::Core only). Hooked at the dispatch
  site *after* `applyEtagPattern` (so the etag is computed on the
  unfiltered canonical body and a narrowed call still 304s — list
  `"etag"` in `fields` to keep it) and *before* the `<ants_mcp_data>`
  wrap. Skipped on the etag short-circuit. Unknown field names are
  dropped (all-unknown ⇒ `{}`); empty/absent `fields` ⇒ full payload.
  To opt a tool in: add it to `mcp::isFieldProjectionTool` + a
  `makeFieldsProp()` line to its schema. The seven are a subset of
  `isEtagSupportedTool`.

- **`get_scrollback` has a since-cursor incremental mode
  (ANTS-1500).** Absent `since_cursor` ⇒ legacy raw-text return
  (backwards-compatible). Present ⇒ JSON envelope shape
  `{ok, content, cursor, cursor_stale, stale_reason?}` where
  `content` is only the bytes appended since the cursor was
  issued, and `cursor` is the new cursor for the next call.
  Cursor encodes `TerminalGrid::scrollbackPushed()` (uint64
  monotonic). Three stale paths surface explicit reasons:
  `malformed_cursor`, `counter_regressed` (terminal restart /
  unrelated session), `ring_wrapped` (gap exceeded
  `maxScrollback()`). All three fall back to the full window so
  the caller never silently loses data.

- **`roadmap_query` now recognises three formats (ANTS-1530).**
  `detectRoadmapFormat` returns one of `ants-v1` (default),
  `github-task-list` (ANTS-1428), or `pass-headings` (ANTS-1530).
  The last triggers on docs with ≥2 `#### Pass N.M …` headings
  and ≥2 `- **Status**: <word>` markers AND no ants-v1 emoji
  bullets — the 2+2 threshold rules out accidental fenced-code
  examples. `parsePassHeadingBullets` synthesises one bullet per
  `#### Pass N.M …` heading with ID `PASS-<major>-<minor>` (so
  the default `[PROJ-NNNN]` filter still surfaces them) and maps
  the status word from the next `- **Status**:` line
  (`todo/planned` → 📋, `in-progress/doing/wip` → 🚧,
  `done/shipped/completed` → ✅, `deferred/considered/parked` →
  💭). `(SEVERITY, SIZE)` meta re-emits in the headline so it
  survives in `headline_oneline`.

- **`session_memory`, `workflow_state` + `project_layout` gate routing
  is asymmetric (ANTS-1336 + ANTS-1435).** Write ops on tenant-hashed
  storage (`session_memory set/delete`, `workflow_state set/clear`) go
  through RcGate — focused-tab match required, prevents the
  confused-deputy attack. Read ops (`session_memory get/list`,
  `workflow_state get`, entire `project_layout`) anchor to `caller_cwd`
  directly — canonicalise + isDir check, no focused-tab match.
  `workflow_state` keys are stored as `wf.<skill>` in the same backing
  file as `session_memory`; 72 h lazy-TTL purge on every `set`.
  ANTS-1723. The storage at
  `~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json` is per-cwd-
  hashed and reads of the caller's own bucket are self-scoped.
  `session_memory`'s legacy `cwd` arg is ignored (still in the
  schema for one release as `DEPRECATED`, drops in 0.7.93);
  `caller_cwd` is the only project-scope source. ANTS-1372 § 4
  INV-7 amended by ANTS-1336 then re-amended by ANTS-1435 to
  reflect the read/write split. See `docs/specs/ANTS-1435.md` and
  its `§Limitations` block for the same-UID observability trade-off.

- **Path-accepting MCP tools route through `PathValidation::validatePath`
  (ANTS-1295).** Every tool that takes a path-typed argument (`path`,
  `lane`, `reports_dir`, `file`) calls
  `PathValidation::validatePath(rawPath, rootCanonical, toolName,
  paramName)` from `src/pathvalidation.h` before any filesystem
  operation. The helper NFC-normalises, rejects control chars +
  backslash, canonicalises (symlink-resolving), and anchors to the
  focused project root. Reject envelope:
  `{ok:false, error:"<tool>: \"<param>\" escapes project root",
  code:"bad_path"}`. On accept, `check.argvForm` is the safe-for-argv
  form (with `./` prefix when the path starts with `-`); `check.resolved`
  is the canonical FS path or empty if the path doesn't exist yet
  (valid for git pathspec on deleted files; callers that require
  existence check `check.resolved.isEmpty()`). If you add a new tool
  that consumes a path, call validatePath at the MCP layer — do NOT
  re-implement the anchor inline. See `docs/specs/ANTS-1295.md`.

## Project standards

Four shareable v1 standards at `docs/standards/`:

- [`coding.md`](docs/standards/coding.md), [`documentation.md`](docs/standards/documentation.md),
  [`testing.md`](docs/standards/testing.md), [`commits.md`](docs/standards/commits.md)
- Sub-spec: [`roadmap-format.md`](docs/standards/roadmap-format.md)
  — stable `[ANTS-NNNN]` IDs from `.roadmap-counter`, status emojis
  (✅ 🚧 📋 💭), theme emojis, position-is-priority, `Kind:` /
  `Source:` taxonomy, fold-in subsections.
- Sub-spec: [`specs.md`](docs/standards/specs.md) (ANTS-1728) —
  spec-authoring standard for `docs/specs/ANTS-NNNN.md`: required
  structure (H1 + Status/Kind/Source header, § 1 Problem, § 2 Surface,
  Invariants, Tests), the bullet `- **INV-N** —` form (96/98 specs;
  the minority GFM-table form is also parsed), grounding/RAM/security
  conventions, cold-eyes loop log, and the `spec_query` machine-
  readability contract. Cite when writing or reviewing a spec.
- Sub-spec: [`mcp-error-codes.md`](docs/standards/mcp-error-codes.md)
  (ANTS-1353) — canonical taxonomy for the `code` field on MCP
  refusal envelopes (`{ok:false, error, code}`). Five categories:
  input validation, resource state, caller-cwd contract, I/O, and
  dispatcher. Cite when adding or reusing a code in a new refusal
  site.
- Sub-spec: [`mcp-caches.md`](docs/standards/mcp-caches.md)
  (ANTS-1439) — keying + relocation contract for every MCP cache.
  The invariant: a path-keyed cache may go cold or orphan across a
  project move but must never *shadow* (serve the old path's data
  under the new path). Inventory table + "adding a new cache"
  checklist. Cite when adding any project-scoped cache.

`coding.md`, `commits.md`, and `testing.md` are byte-identical to
`/start-app`'s template at
`~/.claude/skills/app-workflow/templates/docs/standards/`. The
remaining two carry project-specific additions on top of the
template: `documentation.md` adds § 7 Accessibility (ANTS-1235);
`roadmap-format.md` adds the `Layman:` field (§ 3.5) and the
§ 3.9 archive-rotation block. Project-local additions (not in the
template, not shareable as-is): `status-bar.md` documents this
codebase's status-bar widget convention;
[`audit-false-positives.md`](docs/standards/audit-false-positives.md)
defines the `.ants_review_falsepos.jsonl` ledger contract
(ANTS-1457) shared by `/audit`, `/cold-eyes`, `/indie-review`,
and `/test-audit`. `mcp-errors.md` is an earlier (2026-05-12)
draft kept as a historical reference — `mcp-error-codes.md`
(ANTS-1353) is the authoritative MCP error taxonomy.
[`test-audit-resume.md`](docs/standards/test-audit-resume.md)
(ANTS-1580) documents the client-side `partition_token`
save-and-resume recipe via `session_memory` — the token is
in-process LRU, so resume across an Ants restart needs an
explicit handshake.

ADRs live at `docs/decisions/` (Michael Nygard format); per-feature
specs at `docs/specs/`; per-phase outcomes at `docs/journal/`.
`docs/plans/` is a deprecated location — shipped plans should
migrate to `docs/journal/`, in-flight design notes belong in
`docs/specs/`. Existing `docs/plans/*` are kept as historical
records only.

## Versioning & release files

SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the
single source of truth.** `ANTS_VERSION` macro propagates everywhere
— never hardcode version strings in `.cpp`/`.h`.

Every bump touches `CMakeLists.txt`, `CHANGELOG.md` (new dated
section, Keep-a-Changelog), `README.md` ("Current version" line). Use
the `/bump` skill — its `.claude/bump.json` recipe covers the
packaging files too.

Completed `ROADMAP.md` items migrate to the matching CHANGELOG
section. `PLUGINS.md` is the plugin-author contract — update it in
the same commit when `luaengine` / `pluginmanager` change the
`ants.*` surface.

**Release candidates (ANTS-1318 frozen-RC pipeline).** The weekly
Wednesday cadence cuts a public release plus a Patron-preview RC. The
`-rcN` suffix lives ONLY at the git tag, the GitHub-release title, and
the AppImage filename — never in `CMakeLists.txt`/`bump.json` (INV-3 /
INV-9). RC orchestration is `packaging/cut-rc.sh` (`new-rc` / `respin`
/ `promote` / `status`), NOT the global `/release` skill (which is
shared across projects). Flow: `/bump` to the base `X.Y.Z` first, then
`cut-rc.sh new-rc --push` tags `vX.Y.Z-rcN` + creates a `--prerelease`
GitHub release. `release.yml` routes RC AppImages to their own zsync
channel so stable users can't auto-update onto an RC. Spec
`docs/specs/ANTS-1318.md`.

## Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) for correct line wrapping.
- Alt-screen 1049 supported (vim/htop).
- Combining chars in per-line side table — zero overhead when absent.
- Image paste auto-saves and inserts filepath (Claude Code workflow).
- Lua sandbox strips dangerous globals + instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` config key drives per-pixel terminal-area fillRect alpha
  only; chrome paints opaque via `WA_StyledBackground`. No
  `setWindowOpacity()` path — `background_alpha` was removed as
  redundant in 0.7.18.
- Audit rule pack is JSON not YAML (`QJsonDocument` built-in; flat
  schema). Hardcoded checks stay in C++; `audit_rules.json` only
  appends/overrides.
- Audit uses `clazy-standalone` (Qt-aware AST) not embedded libclang.
- `.audit_suppress` is JSONL v2 (`{key, rule, reason, timestamp}`);
  v1 plain-key lines load and convert on first write.
- Audit external-tool calibration reads **existing** project configs
  rather than adding new suppression files (2026-04-21 audit-hygiene
  report). `.audit_allowlist.json` exists only for custom grep rules
  with no upstream config.
- Audit test harness is shell-based against fixture dirs — no C++
  unit framework, no link-time coupling to `auditdialog`.
- Confidence score (0-100): floor +10 (any signal), `severity×15`,
  +20 cross-tool corroboration (sets `highConfidence` → ★ tag in
  summary table + SARIF property), +10 external AST tool, −5 if
  grep source AND message length < 30 chars, −20 test path.
  AI-triage caps: `FALSE_POSITIVE` clamps ≤ 30, `TRUE_POSITIVE`
  floors ≥ 80.
- SARIF exports include `contextRegion` (±3 lines) + `properties.blame`
  per sarif-tools convention. Generated files (`moc_*`, `ui_*`,
  `qrc_*`, `*.pb.cc/.h`, `/generated/`, `_generated.*`) auto-skipped.
- Roadmap-query IPC verb caches parsed bullets with mtime + 100 ms
  wall-clock TTL (ANTS-1117 INV-10).
