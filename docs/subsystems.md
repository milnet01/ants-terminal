# Ants Terminal — subsystem module map

Canonical per-subsystem reference for `src/`, **served on demand** by the
`subsystem` MCP tool (`op=map`). It deliberately lives **outside CLAUDE.md**
so the lane catalogue is not reloaded into every Claude session preamble
(ANTS-1292) — query it with the `subsystem` tool when you need a lane
summary.

**It is not what `indie_review_partition` returns, and has not been since
ANTS-4793.** That verb prefers `.indie-review/partition.json` when present,
and this project commits one. The two are different partitions on purpose,
at different granularities:

| | this file | `.indie-review/partition.json` |
|---|---|---|
| Serves | `subsystem op=map` — a human reading about one module | `indie_review_partition` — one review lane per entry |
| Granularity | fine, roughly per module | coarse, modules grouped into reviewable lanes |
| Coverage | the modules worth describing | **every** tracked `src/` file |

So a module absent here is not a module absent from review, and a lane name
here need not exist there. Where a reader wants "which lane reviews this
file?", the override answers it; this file answers "what does this module
do?". Keep this one in sync with the code the same way CLAUDE.md used to be
kept in sync.

Format contract (parsed by `SubsystemMap::parse`): the `## Module map
(src/)` H2 below, then one bullet per subsystem shaped as
``- `name` — summary`` (em-dash separator; multi-name and qualifier-paren
forms tolerated; two-space indented continuation lines append to the
summary).

## Module map (src/)

One line each — deep mechanism detail lives in the cited spec / the source.
Listed only where behavior isn't obvious from the name.

- `vtparser` — VT100/xterm state machine. Emits `VtAction` structs
  (Print/Execute/CSI/ESC/OSC/DCS/APC).
- `terminalgrid` — cell grid + scrollback + cursor + modes. Owns OSC 8
  hyperlinks, OSC 52 clipboard, OSC 133 shell integration, OSC 9/777
  notifications, OSC 9;4 progress (peek-disambiguated from OSC 9), Kitty
  keyboard protocol, Sixel/Kitty-APC/iTerm2 images, DA/CPR/DSR response
  callback, per-line `combining` side-table (zero cost when absent),
  soft-wrap reflow on resize.
- `terminalwidget` (QWidget) — QPainter + QTextLayout renderer with
  HarfBuzz ligatures. SGR mouse, focus reporting, sync output, undercurl,
  per-pixel bg alpha. (Was QOpenGLWidget pre-0.7.4; glyph-atlas
  `GlRenderer` retired in 0.7.44.)
- `ptyhandler` — forkpty + QSocketNotifier.
- `auditdialog` — static-analysis panel. Filter→enrich→render pipeline:
  parse → drop/suppress/dedup → comment-string + mypy fold → cap →
  enrichment (snippet ±3, git blame, confidence 0-100) → trend →
  SARIF v2.1.0 / HTML. `.audit_suppress` *marks* findings (SARIF
  `suppressions[]`), doesn't drop. Recognises foreign suppression markers
  (NOLINT, cppcheck-suppress, noqa, nosec, nosemgrep, #gitleaks:allow,
  eslint-disable-*, pylint: disable) + native `// ants-audit: disable[=rule]`.
- `auditengine` (Qt6::Core) — pure-function counterparts of the dialog
  pipeline; non-GUI consumers (CI, ants-helper, MCP) link this without
  Qt6::Widgets. ANTS-1119.
- `auditcache` (Qt6::Core) — per-project `.audit_cache/` for `audit_run`:
  SARIF + `index.json` manifest, atomic via QSaveFile + 0600, retention
  reaper caps history at 10. Spec ANTS-1555.
- `audithygiene` — splices project-local scanner config into invocations
  (`.semgrep.yml` → `--exclude-rule`; `pyproject.toml` ruff S-codes →
  bandit `--skip`).
- `auditautofix` (Qt6::Core, `ants_audit_lib`) — native safe-list
  auto-fixer; `planRepair` returns a behaviour-neutral one-line `Repair`
  only for provable cases (dead `#include`, dead `Q_UNUSED`, expired TODO,
  `//word`→`// word`), atomic write + stale-plan refusal. Opt-in
  "Auto-fix safe" button only, never on a scan. ANTS-1719.
- `falseposledger` (Qt6::Core, `ants_core_lib`) — prose-grain
  false-positive ledger `.ants_review_falsepos.jsonl`, shared by
  /cold-eyes, /indie-review, /test-audit. Read-only in v1 (CC appends).
  Distinct from line-grain `.audit_suppress`. ANTS-1457; standard
  `docs/standards/audit-false-positives.md`.
- `auditfpledger` (Qt6::Core, `ants_core_lib`) — fingerprint-keyed
  learned-FP ledger for static-audit findings (`learned-fp.jsonl`);
  fingerprint is line-INDEPENDENT so a learned FP survives line shifts.
  `applyLearnedFpSuppressions` MARKs (not drops). Code-grain sibling of
  `falseposledger`. ANTS-1708.
- `featurecoverage` — in-process audit lanes via
  `AuditCheck::inProcessRunner`: `spec_code_drift`,
  `contract_doc_drift` (ANTS-3600 — back-ticked literals in
  `docs/standards`/`docs/specs` that no longer appear in project sources),
  `changelog_test_coverage`. (`test_health` is shell-side in
  `auditdialog.cpp`.)
- `markdownscan` (Qt6::Core, `ants_core_lib`) — shared CommonMark fence
  primitives (`fenceRe` / `fenceOpenerChar` / `fenceMask`), hoisted from the
  verbatim copies in `feedbackfile`/`speclog`. Consumed by `feedbackfile`,
  `speclog`, `featurecoverage` (contract_doc_drift), `docintegrity`. ANTS-3603.
- `docfinding` (Qt6::Core, `ants_core_lib`) — the one finding shape the
  doc-lint verbs share: `Finding{verb, kind, file, line, message,
  autoFixable, emissionIndex}`, `toJson` and `countsByVerbAndKind`.
  `emissionIndex` is an in-process sort tiebreak and is never serialised.
  Also fixes the family's engine calling convention (text in, never opens
  the document; anything `ants_core_lib` cannot see is injected via
  `Options`). Blocker for `docdedup`/`docsymbols`/`speclint`, composed by
  `doclint`. ANTS-3664.
- `docintegrity` (Qt6::Core, `ants_core_lib`) — deterministic doc-integrity
  engine: dead anchors, broken relative links, TOC coverage over a doc set.
  GitHub-compatible `gfmSlug`, fence-aware. Powers the `doc_integrity` MCP verb
  and the cold-eyes Phase-1e feed (`doc_integrity[]` in the brief). ANTS-3601.
- `doccitations` (Qt6::Core, `ants_core_lib`) — the citation SCAN layer: what
  does this document cite? A pure function — document text in, citation tokens
  out — with no filesystem, path resolution or status taxonomy; those belong to
  the read path that consumes it. Consumes `MarkdownScan`'s fenceMask +
  codeSpans. Matching is two-stage, a permissive RECOGNISE pass then a VALIDATE
  pass, so a token that fails to match can still be REPORTED rather than
  vanishing. Powers the `doc_citations` MCP verb. ANTS-3653.
- `docsymbols` (Qt6::Core, `ants_core_lib`) — resolves the identifiers a doc
  asserts something about: harvests inline code spans matching
  `ident(::ident)*()?`, applies six exclusions (paths, `doc-examples` regions,
  a short-lowercase floor, language keywords, and the injected MCP
  verb/argument vocabulary), then calls `SymbolQuery::findDefinition` per
  distinct needle. REPORT-ONLY — no severity, nothing auto-fixable; a needle
  the run never looked up is `not_checked`, never `unresolved`. Powers the
  `doc_symbols` MCP verb. ANTS-3661.
- `speclint` (Qt6::Core, `ants_core_lib`) — the greppable half of the
  spec-format contract, which `/cold-eyes` § 1e hand-rolls every pass:
  `invariant_no_test`, `invariant_id_gap`, `loop_row_no_outcome`,
  `command_test_no_expectation` (a candidate — no subprocess is ever run) and
  `missing_section`. Consumes `SpecParse::parseSpecBody` for what each
  invariant says, but owns the id list itself via a fence-aware anchor scan
  confined to the Invariants section — the parser's table branch drops a row
  whose surface cell is empty, which is the very defect the check names.
  Tombstones (`*moved to <ID>*`, `*withdrawn — …*`) are exempt from both
  invariant checks. `missing_section` runs only when the project's format
  standard carries a `<!-- required-sections -->` block; no standard here has
  one, so `sectionsChecked:false` is the shipping default. ANTS-4127 adds
  three more kinds to the same walk — `test_surface_absent`,
  `test_surface_unresolved` and `test_surface_unwired` — resolving a
  `tests/features/<name>` a clause NAMES against disk and against
  `CMakeLists.txt`, bucketed by the spec's own `**Status:**` so only a
  shipped one yields a finding; wildcards are not resolved. Both filesystem
  facts are injected through `Options` like `requiredSections`, so the engine
  still opens nothing, and an empty set means skip rather than fail. Powers
  the `spec_lint` MCP verb. ANTS-3662, ANTS-4127.
- `docdedup` (Qt6::Core, `ants_core_lib`) — near-duplicate passage detection
  across a doc set: paragraph segmentation (a list item is one passage, marker
  line included), word 3-gram shingles, exact Jaccard, and connected-component
  clustering so N docs sharing one stanza are one finding rather than
  N(N−1)/2. Fence-aware via `MarkdownScan`. The family's one **corpus-scoped**
  engine — `Accumulator::add` per document then `finish()` once, because a pair
  needs two documents — and so ANTS-3664 § 2.3's stated exception to the
  per-document signature. Candidate gathering is pruned by `maxPostings`;
  scoring never is, which is the seam INV-1 and INV-7 each govern one half of.
  REPORT-ONLY: which copy is canonical is a judgement, so nothing is ever
  auto-fixable and ANTS-3669 refuses to fix one by name. Powers the `doc_dedup`
  MCP verb. ANTS-3660.
- `focusedtest` (Qt6::Core) — resolves the `focused_test` MCP tool:
  changed files → `ctest -R` patterns via `tests/coverage-map.json`, with
  a basename heuristic + conservative full-suite fallback. Spec ANTS-1302.
- `mcpprojection` (Qt6::Core) — `fields=` response projection for MCP
  read tools; `projectFields`, universal since ANTS-4524 (read the
  predicate for the current allowlist — it grows a verb at a time, and a
  count stated here goes stale on the next addition). ANTS-1720.
- `briefdispatch` (Qt6::Core, `ants_core_lib`) — shared dispatch-brief
  composer for the review-dialog family: `fenceBody` (4-backtick
  fence-hardening kernel), `inlineBodies`, `inlineRelevantSections`
  (keyword-sliced cross-ref docs). ANTS-1727.
- `llmclient` (Qt6::Core+Network, `ants_core_lib`) — OpenAI-compatible
  streaming chat client (request build, SSE drain, 10 MiB caps, scheme
  allowlist, OWASP LLM06 scrub). Static test seams for parse/scrub/cap.
  Drives `AiDialog`'s network. ANTS-1727.
- `llmdispatcher` (Qt6::Core+Network, `ants_core_lib`) —
  bounded-concurrency pool over `LlmClient`; `maxConcurrent` clamped
  `[1,4]` (`ai_review_concurrency`, default 2). ANTS-1727.
- `reviewdialogbase` (`ants_dialogs_lib`) — shared QDialog scaffold for
  the v2 review-dialog family. Owns partition tabs, Dispatch button (→
  `LlmDispatcher`), results host, Fold-into-ROADMAP. Hooks:
  `derivePartition` / `composeBrief` / `onAllReportsCollected` /
  `performFoldIn`. Base provides fold-in *helpers*, not an orchestrator
  (the subclass owns the sequence, so engine-owned fold-in doesn't
  double-allocate). ANTS-1721.
- `coldeyesdialog` (`ants_dialogs_lib`) — native in-app cold-eyes doc
  review over `ColdEyesEngine`. Composes its own inlined-bodies brief
  (not the engine's paths-only manifest), 200 KiB cap; corroborates via
  cross-doc diff. Tools › Review. Spec ANTS-1721.
- `testauditdialog` (`ants_dialogs_lib`) — native in-app test-suite
  review over `TestAuditEngine`. Per-chunk lanes; `briefFor` retries once
  on `stale_partition`; fold-in calls engine `foldIn` DIRECTLY (engine
  owns ID allocation). Resume via `SessionMemoryEngine`. Tools › Review.
  Spec ANTS-1722.
- `indiereviewdialog` (`ants_dialogs_lib`) — native in-app source-code
  review over `IndieReviewEngine` (one lane per module-map subsystem).
  REUSES the engine's `assembleBriefForDispatch` (already FP-injected +
  fenced); only sum-caps. Tools › Review. Spec ANTS-1258.
- `remotecontrol` — Kitty-style JSON-over-Unix-socket IPC. Verbs: `ls`,
  `send-text`, `new-tab`, `select-window`, `set-title`, `get-text`,
  `launch`, `tab-list`, `roadmap-query`, `workspace-search`,
  `file-outline`, `find-definition`, `find-caller`, `similar-code`,
  `git-state`, `subsystem`, `roadmap-branch-drift`. Trust model:
  UID-scoped + 0700 perms + `lstat`-checked `S_ISSOCK`.
  **The class is split across `remotecontrol_*.cpp` translation units
  (ANTS-3833, ANTS-3855, ANTS-4620, ANTS-4622), each with its own entry
  below.** `src/remotecontrol.h` is unchanged; only the bodies were cut, and
  `remotecontrol.cpp` keeps the `dispatch` chain and the shared `rcdetail`
  helper pool. `CMakeLists.txt`'s `ANTS_RC_SOURCES_REL` names the TUs in
  slice order, `ants_core_lib` consumes that list, and the `ANTS_RC_SOURCES`
  compile definition carries the same order to the test tree, where
  `ants_test::slurpRemoteControl()` reads the whole set. A new verb's body
  goes in its family's TU, its `dispatch` routing entry in
  `remotecontrol.cpp`. Cross-TU helpers are declared in
  `src/remotecontrol_internal.h`, which nothing outside the list may include.
  **Position in that list is load-bearing**, and the `TU n/N` header ordinals
  track it: a TU that is a SLICE of the pre-split file is inserted at its
  slice position, one that never existed there is appended, so the
  two-anchor scrape windows the tests use do not silently move.
- `remotecontrol_terminal` — terminal and window verbs: tabs, windows, text
  send and capture, plus the `--e2e` input-injection and screenshot verbs
  (`cmdSendText`, `cmdGetText`, `cmdTabList`, `cmdInjectKey`, `cmdGrabImage`).
- `remotecontrol_roadmap_query` — the roadmap READ surface (bullets, bundles,
  reports) and, despite the name, the `cmdRoadmapLog` op dispatcher plus the
  `rl*` helpers the write TUs share.
- `remotecontrol_changelog` — reads and writes `CHANGELOG.md`: section
  queries, and entry appends singly or in batches.
- `remotecontrol_roadmap_log` — single-item roadmap writes: append a bullet,
  flip its status, amend its body or one trailer column.
- `remotecontrol_roadmap_log_batch` — multi-item roadmap writes and section
  surgery: batch append and flip, bundle rows, create / retitle / rotate a
  section. Cut out of the TU above when it reached ANTS-3833 INV-6's line
  cap, and INSERTED at its slice position rather than appended (ANTS-4620).
- `remotecontrol_workspace` — code navigation and file I/O: search, outline,
  region reads, `apply_edits`, the codebase and build-target index, and
  `mutation_probe`.
- `remotecontrol_docs` — documentation health: the docs index, integrity /
  citation / dedup checks, spec lint and conformance, and the per-project
  `.ants/project.json` settings verb.
- `remotecontrol_feedback` — the cross-session feedback files (read and
  write), spec-file writes, git status / log / diff plumbing, audit
  false-positive bookkeeping, and the `caller_cwd` → project-root resolver
  every verb depends on.
- `remotecontrol_state` — session orientation and "where am I" state (git,
  subsystem, audit, spec), the symbol-lookup verbs, and the per-session
  advisory verbs (`invariant_check`, `task_priors`, `project_conventions`,
  `focused_test`).
- `remotecontrol_review` — the AI-reviewer pipeline (partition, brief,
  dispatch, corroborate, fold in), plus debt-sweep, `verify_changes`,
  `plan_template` and `token_usage`.
- `remotecontrol_coldeyes` — the cold-eyes document-review verbs, alongside
  session memory, workflow / layout state, and build and test reporting.
- `remotecontrol_roadmap_backfill` — one-off git walk that dates roadmap rows
  predating forward ship-date stamping (`roadmap_log op:"backfill_dates"`).
- `remotecontrol_roadmap_repair` — recovers trailer values migration cut
  short, by re-parsing each bullet's stored prose (`op:"repair_trailers"`).
- `remotecontrol_roadmap_publish` — publishes the store to `ROADMAP.md` with
  no semantic change (`op:"render"`).
- `remotecontrol_session_message` — thin handler for the cross-session
  mailbox verb; the store does the work.
  (`remotecontrol_roadmap_migrate` is the remaining TU and has its own entry
  further down.)
- `antshelper` (optional CLI, `-DANTS_ENABLE_HELPER_CLI=ON`) — local
  subagent for Claude Code; v1 surface is `drift-check`. ANTS-1116.
- `luaengine` / `pluginmanager` — sandboxed Lua 5.4; plugins in
  `~/.config/ants-terminal/plugins/`, gated by `ANTS_LUA_PLUGINS`.
  **Per-plugin worker-thread model (ANTS-1750):** each `LuaEngine` runs
  on its own `QThread`; the `lua_State` is created and only ever touched
  on that worker. Every GUI→worker edge is `QueuedConnection` (the GUI
  never blocks); the one worker→GUI read (`ants.settings.get`) is
  `BlockingQueuedConnection` → acyclic, no deadlock. **Route
  keybinding/palette through `dispatchTo`, never
  `engineFor()->fireEvent()`.** A 2 s `healthTick` demotes a plugin past
  budget (atomic `m_abortRequested`); a wedged worker is detached into
  `m_zombies` (never joined). Spec ANTS-1750; veto contract ANTS-1736
  §2.6; process-isolation follow-up ANTS-1795.
- `claudeintegration` — singleton owning the Claude Code hook server
  (one UDS shared across all tabs), `m_pollTimer` (2 s) PID detection,
  `sessionPathForCwd` (project-scoped JSONL resolver, ANTS-1163),
  `processHookEvent` with the `isFocusedTabSession` gate (ANTS-1161 drops
  foreign-tab events). PermissionRequest routes via `m_lastHookSessionId`,
  not the gate.
- `claudestatuswidgets` — `ClaudeStatusBarController` owns the bottom-bar
  Claude chips (review-changes, bg-tasks, tasks, context-bar, error,
  model-recommender). The repo-visibility label lives in `MainWindow`;
  "audit" is a `m_statusLabel` state, not a chip. Refreshers fire on the
  2 s status timer + `poll()` / `sweepLiveness()` for watch-loss recovery.
  The bottom `Claude: <state>` label and the per-tab dot read through
  the shared `claudestateresolver` helper (ANTS-1873) so the two
  surfaces cannot disagree. ANTS-1894 — also emits near-miss records
  via `maybeEmitNearMiss` (sole producer for `modelnearmissledger`),
  using two process-local `QHash<project, …>` maps to throttle by
  signature change.
- `claudestateresolver` (Qt6::Core, `ants_claude_lib`) — single
  source-of-truth helper for the focused Claude session's display
  state. `Resolved` value (`base/tool/planMode/auditing/awaitingInput`)
  + `Display` precedence ladder
  (`awaitingInput → planMode → auditing → base`), consumed by
  `claudestatuswidgets`' tab-dot lambda + `apply()` and (in due
  course) `modelautoswitch`'s INV-2 gate. ANTS-1873.
- `modelrecommender` (Qt6::Core) — stateless `score(transcriptPath)`:
  tail-reads ≤ 512 KB JSONL, scores the last 20 assistant turns
  (file-writes, tool diversity, plan keywords, length), returns a `Result`
  struct (`Tier` + reason + `currentModel` + `isMechanical`). ANTS-1944 —
  `Result` also carries `currentModelFromCommand` (true when currentModel
  came from a `/model` command, i.e. ANTS-1916 authoritative read) and
  `currentModelTsMs` (assistant-turn timestamp in ms, 0 on command/absent
  timestamp) so the gate can reconcile a stale transcript read against the
  actuator's actual last-set tier. No side effects. ANTS-1226. Sibling
  `thinkingLevelFromLatestUserTurn` (ANTS-1888) is a pure helper for the
  passive model-state chip — same tail-read pattern, but walks for the
  most recent `{type:"user"}` line and matches the inline thinking
  directive set (`ultrathink` / `think harder` / `think hard` / `think`
  / `/nothink` → Standard). Scorer-v2 (ANTS-1890, shipped 2026-05-26) adds two pure helpers:
  `hasCommitIntent` (commit-intent hard override on the latest user turn
  — stem-regex over commit/push/stage/bump/rebase with per-keyword
  English morphology) and `weightForTurnIndex` (1.0×→3.0× linear
  recency weighting applied to fileWriteCount + avgLen; threshold
  raised 4→8 to compensate for the doubled mass). `score()` now also
  walks for the latest user-turn text in the same single pass (no extra
  tail-read) and skips tool_result-only user lines.
- `modelautoswitch` (Qt6::Core, `ants_claude_lib`) — pure decision helper
  for the autonomous switcher (Shape B): `clampToFloor` + `decide(Gate)` +
  `reconcileCurrentTier` (ANTS-1944 — anchors the gate's `current` to the
  actuator's last-set tier when the transcript is stale, but ONLY to suppress
  re-firing the same tier; never overrides toward a user-picked tier).
  gate (enabled / focused-tab Idle / composer-empty / clamped-target
  hysteresis / stability / dwell / per-project override cool-down →
  lowercase tier alias via `ModelRecommender::tierName`). In claude_lib
  (not core) so the `tierName` reuse doesn't invert the layer DAG. The
  live actuator (`ClaudeStatusBarController::refreshAutoModelSwitch`,
  timer-driven) injects `/model <tier>\n` on `decide(...).act`, appends
  a pending ledger record, and suppresses the Shape A chip when enabled
  (INV-14). The per-project cool-down (`Gate::msSinceLastOverride` +
  `kOverrideCooldownMs=10min`, ANTS-1890) is fed by a controller-side
  `QHash<QString, qint64> m_lastOverrideMsByProject` cache —
  bootstrap-seeded from the ledger at `attach()` (restart-safe) and
  incrementally populated by `fillPendingLedgerOutcomes` after each
  settled `userOverrideWithin5`. Default-OFF via
  `Config::claudeAutoModel().switch_enabled`; S2 (live composer-empty
  proxy validation) gates the default-ON flip. ANTS-1735 + ANTS-1890.
- `modelswitchledger` (Qt6::Core, `ants_core_lib`) — model-switch
  effectiveness ledger: JSONL append + 256 KiB drop-oldest eviction with
  pending-record pinning (atomic, 0600), plus pure outcome detection
  (user-override / under-route / correction) and `statsEnvelope`
  aggregation (avoided-Opus vs regret/under-route ratio). `Record` carries
  an `epoch` int field (ANTS-1941, default 0 = pre-epoch); `StatsConfig`
  has `minEpoch` to filter only current-behaviour records. Read by the
  `model_switch_stats` MCP verb. In core so the mainwindow dispatch can
  reach it. ANTS-1735.
- `modelnearmissledger` (Qt6::Core, `ants_core_lib`) — near-miss
  sibling to `modelswitchledger`: records gate-evaluated-but-blocked
  auto-switch decisions to `~/.cache/ants-terminal/model-switch-nearmiss.jsonl`
  (256 KiB cap, drop-oldest, 0600, no pending-pinning) with the
  7-token `blocked_by` taxonomy (`auto_switch_disabled`,
  `focused_state_not_idle`, `composer_not_empty`,
  `target_equals_current`, `ticks_target_stable_insufficient`,
  `dwell_time_insufficient`, `override_cooldown_active`).
  Emit-on-signature-change with a 5 s per-project floor (controller-
  side throttle on `ClaudeStatusBarController`). Aggregated by
  `model_switch_stats` — slim `near_misses` block in default mode,
  full breakdown via `mode:"near_misses"`. Reuses
  `ModelSwitchLedger::nowIso8601` / `parseIso8601Ms` for timestamps.
  ANTS-1894.
- `roadmapdialog` — ROADMAP.md viewer. `renderCardsHtml` (v2 card
  renderer, in use); `renderHtml` is **test-only** (no prod callers since
  ANTS-1747; its suite locks shared filter/sort/anchor/TOC semantics).
  `roadmap-query` IPC uses `parseBullets` + `RoadmapIndex`, not the
  renderer. Section collapse via `ants://expand-section/<slug>`; state
  persists in nine `Config::roadmap*` keys. Spec ANTS-1154.
- `roadmapparse` (`ants_roadmapparse_lib`) — the roadmap markdown reader
  for all three formats (`detectRoadmapFormat` / `parseBullets`), lifted
  out of `roadmapdialog` by ANTS-3764 so headless callers share one
  implementation. Qt6::Core only, in its own leaf library since ANTS-3808
  so the store's headless path reaches the grammar without dragging
  `ants_core_lib`'s Widgets/Network/DBus surface. Owns the ONLY bullet
  grammar: `parseAntsV1Bullet()` (ANTS-3793) hands one bullet's worth of
  it to the store reader rather than letting that reader grow a copy.
- `roadmapstore` (`ants_roadmapstore_lib`) — the store itself: engine,
  location, schema and connection pragmas. It is PRIMARY, not a cache —
  it lives under `XDG_DATA_HOME` (never a cache path), is created
  `synchronous=FULL` and mode 0600, and its only rebuild path is the
  export. Spec ANTS-3756.
- `roadmaprender` (`ants_roadmapstore_lib`) — the inverse of the
  migration: generates `ROADMAP.md` from the store at full fidelity, every
  item in `roadmap-format.md` § 3.5 bullet form, so the generated file is
  the file that used to be written by hand. Lossy in MEMBERSHIP only
  (`internal` and `dropped` are excluded), never in detail. Qt6::Core +
  Qt6::Sql only, so a headless publish path can call it. Spec ANTS-3758.
- `roadmapexport` (`ants_roadmapstore_lib`) — the DURABLE RECORD: the
  store is untracked and the published render is lossy, so the JSONL
  export is the only complete copy that survives a lost disk. Reader and
  writer live together because INV-1 is a round trip (export → rebuild →
  re-export → byte-identical) and neither half is testable alone. Spec
  ANTS-3761.
- `roadmapsource` (`ants_roadmapstore_lib`) — the read seam: the same
  `BulletRecord`s a consumer would have parsed out of `ROADMAP.md`,
  sourced from the store instead. `bulletsFor()` dispatches on a project
  row existing AND its roadmap being recognisably `ants-v1`, with three
  outcomes — store, markdown, or refuse, never a silent fallback;
  `bulletsFromStore()` walks sections in `sectionOrderLess()` order and
  builds each record by parsing `RoadmapRender::bulletText()`, so the two
  backends agree by construction. Refuses over 3,500 items. Spec
  ANTS-3793.
- `roadmapwrite` (`ants_roadmapstore_lib`) — the write seam's ordering,
  in one function because eight `roadmap_log` ops would each get a chance
  to write it differently. `commitAndRender()` is begin → mutate → **dry**
  render → commit → real render: the render commits its own files, so
  validating with the real one would leave a file ahead of a store that
  then rolled back, and `Options::dryRun` is what makes the safe order
  expressible. Six outcomes, each with its own refusal code
  (`render_gate_unmet` / `render_would_drop` / `render_failed` /
  `store_failed` / `write_failed`). `render_would_drop` is ANTS-4141's
  divergence guard, between the dry render and the commit: the design
  assumes the store is a superset of the file, and where it is not the
  render DELETES what the store never imported. The one open window is
  deliberate: a failed *publish*
  leaves the store committed and the file stale-behind, because the store
  is primary. Spec ANTS-3809.
- `roadmapmigrate` (`ants_core_lib`) — the migration read half:
  `findRoadmaps()` resolves a project's roadmap case-insensitively,
  decodes it, and adds every rotated archive under `docs/roadmap/`
  beside it, detecting each source's format separately; the pure
  `planFrom()` turns that `Discovery` into one `MigrationPlan`
  (sections / items / elements / legend / notes) for ANTS-3765 to load.
  Archive sections are namespaced per source file, so a live slug never
  moves. Calls `roadmapparse` for bullets and owns the structural walk
  that reader never had. Specs ANTS-3757 and ANTS-3766.
- `roadmapmigrateload` (`ants_roadmapstore_lib`) — the migration load
  half: `load()` writes one `MigrationPlan` into the store as **one
  project, one transaction**. In the store lib and not `ants_core_lib`
  because it needs Qt6::Sql — that split is the read/load seam made
  mechanical. Matches a re-run on `(project_id, id_fold)`, or on a
  natural key (same section + migration-allocated id + identical
  headline) for the ~40% of the corpus with no id; writes only the
  fields that differ; rebuilds each section's `element` rows rather
  than shifting positions in place; retains, re-files and reports an
  item absent from source; allocates ids inside the transaction so a
  rolled-back load burns none. Refuses an `Access::Interactive` store.
  Spec ANTS-3765.
- `remotecontrol_roadmap_migrate` (`ants_core_lib`) — the `roadmap_migrate`
  verb, and the ONLY production entry point into the three subsystems above:
  before it, the whole migration engine was shipped and reachable from tests
  only. `RemoteControl::cmdRoadmapMigrate` resolves the caller's root, stamps
  the clock once and names `RoadmapStore::defaultPath()`; the free
  `RoadmapMigrateVerb::run(storePath, req)` does everything else, taking the
  path as a parameter so a test can drive it against a `QTemporaryDir` instead
  of the developer's real store. Opens its OWN `Access::Bulk` connection for
  the call and releases it before returning — never the process-owned
  `Interactive` one, which `load()` refuses. `dry_run` previews the counts and
  rolls back, but does open (and may create) the store. Spec ANTS-3855.
- `claudetasklist` / `claudebgtasks` — per-tab JSONL trackers;
  `QFileSystemWatcher` + `poll()` mtime rescue for tmpfile+rename
  rewrites (which drop the watch). `sweepLiveness()` is bg-only. Parser:
  `TodoWrite` (snapshot replace), `TaskCreate` + tool_result (add),
  `TaskUpdate` (status flip). Chip reads `done/total` (ANTS-1246). Both
  filter `isSidechain` + reset on `isCompactSummary`.
