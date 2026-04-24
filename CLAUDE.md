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
- `terminalwidget` (QOpenGLWidget) — two render paths: QPainter+QTextLayout
  (HarfBuzz ligatures) or `glrenderer` (glyph atlas, GLSL 3.3, instanced
  quads). Handles SGR mouse, focus reporting, sync output, undercurl,
  per-pixel bg alpha.
- `ptyhandler` — forkpty + QSocketNotifier.
- `auditdialog` — static-analysis panel. Pipeline:
  `OutputFilter → parseFindings → dedup(SHA-256) → generated-file skip →
   path_rules severity shifts → inline suppression → .audit_suppress →
   baseline diff → trend → enrichment (snippet ±3, git blame,
   confidence 0-100) → render + SARIF v2.1.0 / HTML`.
  Recognizes foreign suppression markers (NOLINT, cppcheck-suppress,
  noqa, nosec, nosemgrep, #gitleaks:allow, eslint-disable-*,
  pylint: disable) plus native `// ants-audit: disable[=rule]`.
- `audithygiene` — splices project-local scanner config into invocations
  (`.semgrep.yml` header → `--exclude-rule`; `pyproject.toml` ruff S-codes
  → bandit `--skip B<nnn>`).
- `featurecoverage` — in-process audit lanes via `AuditCheck::inProcessRunner`
  (no QProcess). Three: `spec_code_drift`, `changelog_test_coverage`,
  `test_health`.
- `luaengine` / `pluginmanager` — sandboxed Lua 5.4; plugins live in
  `~/.config/ants-terminal/plugins/`, gated by `ANTS_LUA_PLUGINS`.

## Data flow

`PTY → VtParser → TerminalGrid → TerminalWidget`
Reverse (DA/CPR/DSR): `TerminalGrid → ResponseCallback → PTY`

## Build

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

Optional deps — each audit check probes with `which <tool>` and
self-disables if absent. No errors on missing: `clazy`, `semgrep`,
`osv-scanner`, `trufflehog` (default OFF — verification hits network),
`hadolint`, `checkov`, `ast-grep`, `cppcheck`, `clang-tidy`,
`shellcheck`, `pylint`, `bandit`, `ruff`. `clazy` needs
`build*/compile_commands.json` (on by default via
`CMAKE_EXPORT_COMPILE_COMMANDS`).

## Testing

```bash
cd build && ctest --output-on-failure
```

**`audit_rule_fixtures`** — `tests/audit_self_test.sh` matches rule
regexes against `tests/audit_fixtures/<rule>/bad.*` (expect N hits with
`// @expect <rule-id>` markers) and `good.*` (expect zero). Count-based,
not line-number-based.

**Feature-conformance** (`tests/features/*`, label `features`) — each
subdir pairs `spec.md` (human contract) with a standalone C++ test
linking only the `src/*.cpp` objects it exercises (GUI-free). To add:

1. Write `spec.md` first (surface to user for sign-off before coding).
2. Write `test_<feature>.cpp` — exit 0/non-zero, print enough on failure
   to diagnose without reproducing.
3. Wire in `CMakeLists.txt` via `add_executable` + `add_test` with
   label `features;fast`.
4. **Verify the test fails against pre-fix code** (`git checkout <sha>
   -- src/...`) before restoring the fix — prevents tests that pass on
   broken code.

See `tests/features/README.md`.

### Cppcheck gotcha

Must pass `--library=qt` or cppcheck misparses `emit` as a type and
flags every signal emission:

```bash
cppcheck --enable=all --std=c++20 --library=qt \
  --suppress=missingIncludeSystem --suppress=unusedFunction \
  --suppress=unknownMacro -I src src/
```

## Conventions

- Signals/slots for cross-component comms.
- Config at `~/.config/ants-terminal/config.json` with 0600 perms.
- Scrollback default 50k, max 1M.
- Theme colors set on `TerminalGrid`; ANSI palette (16+216+24) lives there.
- QTextLayout for ligature shaping.

## Versioning & release files

SemVer. **`project(... VERSION X.Y.Z)` in `CMakeLists.txt` is the single
source of truth.** `ANTS_VERSION` macro propagates everywhere — never
hardcode version strings in `.cpp`/`.h`.

Every bump touches: `CMakeLists.txt`, `CHANGELOG.md` (new dated section,
Keep-a-Changelog), `README.md` ("Current version" line). Use the
`/bump` skill — its `.claude/bump.json` recipe covers the packaging
files too.

Completed `ROADMAP.md` items migrate to the matching CHANGELOG section.
`PLUGINS.md` is the plugin-author contract — update it in the same
commit when `luaengine` / `pluginmanager` change the `ants.*` surface.

## Key design decisions (non-obvious)

- Custom VT100 parser, no pyte/libvterm. Qt6 is the only runtime dep.
- Delayed-wrap (xterm-style) for correct line wrapping.
- Alt-screen 1049 supported (vim/htop).
- Combining chars in per-line side table — zero overhead when absent.
- Image paste auto-saves and inserts filepath (Claude Code workflow).
- Lua sandbox strips dangerous globals + instruction-count timeout.
- Session persistence via `QDataStream` + `qCompress`.
- `opacity` config key drives per-pixel terminal-area fillRect alpha
  only; chrome (title bar, menus, tabs, status bar) always paints
  opaque via `WA_StyledBackground`. There is no separate whole-window
  `setWindowOpacity()` path — prior `background_alpha` config key was
  removed as redundant in 0.7.18.
- Audit rule pack is JSON not YAML (`QJsonDocument` built-in; flat schema).
  Hardcoded checks stay in C++; `audit_rules.json` only appends/overrides.
- Audit uses `clazy-standalone` (Qt-aware AST) not embedded libclang.
- `.audit_suppress` is JSONL v2 (`{key, rule, reason, timestamp}`); v1
  plain-key lines load and convert on first write.
- Audit external-tool calibration reads **existing** project configs
  rather than adding new suppression files (rationale: 2026-04-21
  audit-hygiene report — noise was already documented upstream).
  `.audit_allowlist.json` exists only for custom grep rules with no
  upstream config.
- Audit test harness is shell-based against fixture dirs — no C++ unit
  framework, no link-time coupling to `auditdialog`.
- Confidence score (0-100): base = severity×15, +20 cross-tool, +10
  external AST tool, −20 test path, adjusted by AI-triage verdicts.
  Replaces the old binary `highConfidence` flag.
- SARIF exports include `contextRegion` (±3 lines) + `properties.blame`
  per sarif-tools convention. Generated files (`moc_*`, `ui_*`, `qrc_*`,
  `*.pb.cc/.h`, `/generated/`, `_generated.*`) auto-skipped.
