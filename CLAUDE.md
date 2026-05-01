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
- `auditdialog` — static-analysis panel. Pipeline (ANTS-1136
  doc-fix 2026-05-01: order corrected to match
  `handleCheckOutput` reality):
  `OutputFilter → parseFindings → mark .audit_suppress → drop
   generated-file → drop/shift path_rules → drop allowlist →
   drop inline-suppress → drop non-recent → dedup → cap →
   comment/string filter → mypy-stub fold → enrichment (snippet
   ±3, git blame, confidence 0-100) → trend → render + SARIF
   v2.1.0 / HTML`. (`.audit_suppress` *marks* `f.suppressed=true`
   so SARIF `result.suppressions[]` § 3.34 can surface them; it
   doesn't drop. Dedup runs after the drop steps so we don't
   double-count work on findings that would have been dropped
   anyway.)
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

## Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

## Build & test

```bash
mkdir build && cd build && cmake .. && make -j$(nproc) && ctest --output-on-failure
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

These five files are byte-identical to the `/start-app` template at
`~/.claude/skills/app-workflow/templates/docs/standards/` so projects
scaffolded by the skill share one source of truth. This codebase
predates the skills and follows the standards directly.

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
- Confidence score (0-100): floor +10 (any signal at all),
  `severity×15`, +20 cross-tool corroboration (sets the
  `highConfidence` flag — still live as the ★ tag in the summary
  table and SARIF property), +10 external AST tool, −5 if source
  is grep AND message length < 30 chars, −20 test path. AI-triage
  verdicts cap the score: `FALSE_POSITIVE` clamps ≤ 30,
  `TRUE_POSITIVE` floors ≥ 80. (ANTS-1136 doc-fix 2026-05-01:
  full formula spelled out; pre-fix line omitted floor / grep-
  short / AI caps.)
- SARIF exports include `contextRegion` (±3 lines) + `properties.blame`
  per sarif-tools convention. Generated files (`moc_*`, `ui_*`,
  `qrc_*`, `*.pb.cc/.h`, `/generated/`, `_generated.*`) auto-skipped.
- Roadmap-query IPC verb caches parsed bullets with mtime + 100 ms
  wall-clock TTL (ANTS-1117 INV-10).
