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
  one, so `sectionsChecked:false` is the shipping default. Powers the
  `spec_lint` MCP verb. ANTS-3662.
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
  **Eleven translation units (ANTS-3833).** The class and
  `src/remotecontrol.h` are unchanged; only the bodies were cut, each TU one
  contiguous slice of the old file: `remotecontrol.cpp` (the dispatcher and
  the shared `rcdetail` helper pool), then `_terminal`, `_roadmap_query`,
  `_changelog`, `_roadmap_log`, `_workspace`, `_docs`, `_feedback`, `_state`,
  `_review`, `_coldeyes`. `CMakeLists.txt`'s `ANTS_RC_SOURCES_REL` names them
  in slice order and `ants_core_lib` consumes that list; the
  `ANTS_RC_SOURCES` compile definition carries the same order to the test
  tree, where `ants_test::slurpRemoteControl()` reads all eleven. A new verb's
  body goes in its family's TU, its `dispatch` routing entry in
  `remotecontrol.cpp`. Cross-TU helpers are declared in
  `src/remotecontrol_internal.h`, which nothing outside the list may include.
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
  expressible. Five outcomes, each with its own refusal code
  (`render_gate_unmet` / `render_failed` / `store_failed` /
  `write_failed`). The one open window is deliberate: a failed *publish*
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
- `claudetasklist` / `claudebgtasks` — per-tab JSONL trackers;
  `QFileSystemWatcher` + `poll()` mtime rescue for tmpfile+rename
  rewrites (which drop the watch). `sweepLiveness()` is bg-only. Parser:
  `TodoWrite` (snapshot replace), `TaskCreate` + tool_result (add),
  `TaskUpdate` (status flip). Chip reads `done/total` (ANTS-1246). Both
  filter `isSidechain` + reset on `isCompactSummary`.
