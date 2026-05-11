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
- `terminalwidget` (QOpenGLWidget) — QPainter + QTextLayout renderer
  with HarfBuzz ligatures. SGR mouse, focus reporting, sync output,
  undercurl, per-pixel bg alpha. (The dormant glyph-atlas
  `GlRenderer` was retired in 0.7.44.)
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
- `audithygiene` — splices project-local scanner config into invocations
  (`.semgrep.yml` header → `--exclude-rule`; `pyproject.toml` ruff S-codes
  → bandit `--skip B<nnn>`).
- `featurecoverage` — in-process audit lanes via `AuditCheck::inProcessRunner`
  (no QProcess). Three: `spec_code_drift`, `changelog_test_coverage`,
  `test_health`.
- `remotecontrol` — Kitty-style JSON-over-Unix-socket IPC. Verbs:
  `ls`, `send-text`, `new-tab`, `select-window`, `set-title`,
  `get-text`, `launch`, `tab-list`, `roadmap-query`. Trust model:
  UID-scoped + 0700 perms + `lstat`-checked `S_ISSOCK`.
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
  context-bar, error, repo). `refreshTasksButton` /
  `refreshBgTasksButton` fire on the 2 s status timer, call
  `activeSessionPath(focusedCwd)`, push the path to `m_tasks` /
  `m_bgTasks` only on change, and call `poll()` / `sweepLiveness()`
  for atomic-rewrite watch-loss recovery. `resetForTabSwitch`
  clears trackers synchronously on tab change.
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
  (which silently drops the watch). `setTranscriptPath` is
  idempotent on same path, otherwise removes/re-adds watch and
  calls `rescan()` synchronously. Parser: `TodoWrite` (snapshot
  replace), `TaskCreate` + paired tool_result (incremental add),
  `TaskUpdate` (status flip). `unfinishedCount() = pending only`
  (ANTS-1221, post-1216 refinement).

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
cmake --build build --quiet 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

This keeps a 10k-line build log out of the assistant's context window
while preserving the tail where compile/link failures actually surface.

### CMake presets

`CMakePresets.json` ships three presets:

| Preset | Use |
|---|---|
| `default` | Release + Ninja in `build/`. Honours the in-tree JOB_POOLS cap. |
| `workstation` | Release in `build-workstation/` hard-capped at `-j3` for constrained hardware *or* when the build is competing with a heavy desktop session. Pair with `cmake --build --preset=workstation`. |
| `debug` | Debug + ASan/UBSan in `build-asan/` with sanitizer env vars wired into `ctest --preset=debug`. |

```bash
cmake --preset=default    && cmake --build --preset=default    && ctest --preset=default
cmake --preset=workstation && cmake --build --preset=workstation && ctest --preset=workstation
cmake --preset=debug      && cmake --build --preset=debug      && ctest --preset=debug
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

## Project standards

Four shareable v1 standards at `docs/standards/`:

- [`coding.md`](docs/standards/coding.md), [`documentation.md`](docs/standards/documentation.md),
  [`testing.md`](docs/standards/testing.md), [`commits.md`](docs/standards/commits.md)
- Sub-spec: [`roadmap-format.md`](docs/standards/roadmap-format.md)
  — stable `[ANTS-NNNN]` IDs from `.roadmap-counter`, status emojis
  (✅ 🚧 📋 💭), theme emojis, position-is-priority, `Kind:` /
  `Source:` taxonomy, fold-in subsections.

These files are byte-identical to `/start-app`'s template at
`~/.claude/skills/app-workflow/templates/docs/standards/`; this
codebase predates the skill and follows them directly.

ADRs live at `docs/decisions/` (Michael Nygard format); per-feature
specs at `docs/specs/`; per-phase outcomes at `docs/journal/`.

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
