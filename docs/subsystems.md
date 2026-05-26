# Ants Terminal — subsystem module map

Canonical per-subsystem reference for `src/`. This file is the single
source of truth for the module map; it is **served on demand** by the
`subsystem` MCP tool (`op=map`) and consumed by `indie_review_partition`
(one review lane per entry). It deliberately lives **outside CLAUDE.md**
so the ~130-line lane catalogue is not reloaded into every Claude session
preamble (ANTS-1292) — query it with the `subsystem` tool when you need a
lane summary.

Format contract (parsed by `SubsystemMap::parse`): the `## Module map
(src/)` H2 below, then one bullet per subsystem shaped as
``- `name` — summary`` (em-dash separator; multi-name and qualifier-paren
forms tolerated; two-space indented continuation lines append to the
summary). Keep this in sync with the code the same way CLAUDE.md used to
be kept in sync.

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
  `changelog_test_coverage`. (`test_health` is shell-side in
  `auditdialog.cpp`.)
- `focusedtest` (Qt6::Core) — resolves the `focused_test` MCP tool:
  changed files → `ctest -R` patterns via `tests/coverage-map.json`, with
  a basename heuristic + conservative full-suite fallback. Spec ANTS-1302.
- `mcpprojection` (Qt6::Core) — `fields=` response projection for MCP
  read tools; `projectFields` + `isFieldProjectionTool` (7-tool
  allowlist). ANTS-1720.
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
  surfaces cannot disagree.
- `claudestateresolver` (Qt6::Core, `ants_claude_lib`) — single
  source-of-truth helper for the focused Claude session's display
  state. `Resolved` value (`base/tool/planMode/auditing/awaitingInput`)
  + `Display` precedence ladder
  (`awaitingInput → planMode → auditing → base`), consumed by
  `claudestatuswidgets`' tab-dot lambda + `apply()` and (in due
  course) `modelautoswitch`'s INV-2 gate. ANTS-1873.
- `modelrecommender` (Qt6::Core) — stateless `score(transcriptPath)`:
  tail-reads ≤ 512 KB JSONL, scores the last 20 assistant turns
  (file-writes, tool diversity, plan keywords, length), returns a `Tier`
  (Haiku/Sonnet/Opus) + reason. No side effects. ANTS-1226. Sibling
  `thinkingLevelFromLatestUserTurn` (ANTS-1888) is a pure helper for the
  passive model-state chip — same tail-read pattern, but walks for the
  most recent `{type:"user"}` line and matches the inline thinking
  directive set (`ultrathink` / `think harder` / `think hard` / `think`
  / `/nothink` → Standard).
- `modelautoswitch` (Qt6::Core, `ants_claude_lib`) — pure decision helper
  for the autonomous switcher (Shape B): `clampToFloor` + `decide(Gate)`
  gate (enabled / focused-tab Idle / composer-empty / clamped-target
  hysteresis / stability / dwell → lowercase tier alias via
  `ModelRecommender::tierName`). In claude_lib (not core) so the
  `tierName` reuse doesn't invert the layer DAG. The live actuator
  (`ClaudeStatusBarController::refreshAutoModelSwitch`, timer-driven)
  injects `/model <tier>\n` on `decide(...).act`, appends a pending
  ledger record, and suppresses the Shape A chip when enabled (INV-14).
  Default-OFF via `Config::claudeAutoModel().switch_enabled`; S2 (live
  composer-empty proxy validation) gates the default-ON flip. ANTS-1735.
- `modelswitchledger` (Qt6::Core, `ants_core_lib`) — model-switch
  effectiveness ledger: JSONL append + 256 KiB drop-oldest eviction with
  pending-record pinning (atomic, 0600), plus pure outcome detection
  (user-override / under-route / correction) and `statsEnvelope`
  aggregation (avoided-Opus vs regret/under-route ratio). Read by the
  `model_switch_stats` MCP verb. In core so the mainwindow dispatch can
  reach it. ANTS-1735.
- `roadmapdialog` — ROADMAP.md viewer. `renderCardsHtml` (v2 card
  renderer, in use); `renderHtml` is **test-only** (no prod callers since
  ANTS-1747; its suite locks shared filter/sort/anchor/TOC semantics).
  `roadmap-query` IPC uses `parseBullets` + `RoadmapIndex`, not the
  renderer. Section collapse via `ants://expand-section/<slug>`; state
  persists in nine `Config::roadmap*` keys. Spec ANTS-1154.
- `claudetasklist` / `claudebgtasks` — per-tab JSONL trackers;
  `QFileSystemWatcher` + `poll()` mtime rescue for tmpfile+rename
  rewrites (which drop the watch). `sweepLiveness()` is bg-only. Parser:
  `TodoWrite` (snapshot replace), `TaskCreate` + tool_result (add),
  `TaskUpdate` (status flip). Chip reads `done/total` (ANTS-1246). Both
  filter `isSidechain` + reset on `isCompactSummary`.
