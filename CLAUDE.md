# Ants Terminal

A real terminal emulator built from scratch in C++ with Qt6.

## Tech Stack

- **C++20** with **Qt6** (Widgets, Core, Gui, Network, OpenGL, OpenGLWidgets)
- **Lua 5.4** (optional, for plugin system)
- **libutil** for PTY (forkpty)
- CMake build system

## Project Structure

```
├── CMakeLists.txt
├── src/
│   ├── main.cpp              # Entry point, OpenGL surface format setup
│   ├── mainwindow.h/.cpp     # Main window, menus, theme switching, dialog lifecycle
│   ├── terminalwidget.h/.cpp # QOpenGLWidget: renders grid, handles keyboard input
│   ├── terminalgrid.h/.cpp   # Character grid, scrollback, processes VtActions
│   ├── vtparser.h/.cpp       # VT100/xterm escape sequence state machine
│   ├── ptyhandler.h/.cpp     # PTY management (forkpty, read/write, resize)
│   ├── themes.h/.cpp         # 11 color themes with ANSI palette overrides
│   ├── config.h/.cpp         # Persistent JSON config (all settings)
│   ├── commandpalette.h/.cpp # Ctrl+Shift+P searchable command overlay
│   ├── glrenderer.h/.cpp     # OpenGL glyph atlas + shader-based renderer
│   ├── sessionmanager.h/.cpp # Session save/restore (scrollback serialization)
│   ├── aidialog.h/.cpp       # AI assistant dialog (OpenAI-compatible API)
│   ├── sshdialog.h/.cpp      # SSH manager dialog (bookmarks + connection)
│   ├── settingsdialog.h/.cpp # Settings dialog (all preferences)
│   ├── titlebar.h/.cpp       # Custom frameless title bar
│   ├── toggleswitch.h/.cpp   # Custom toggle switch widget
│   ├── auditdialog.h/.cpp    # Project audit: clazy + cppcheck + grep checks,
│   │                         # SARIF v2.1.0 + HTML export, JSONL suppressions,
│   │                         # baseline/trend, user rules (audit_rules.json)
│   ├── audithygiene.h/.cpp   # Project-local scanner calibration parsers
│   │                         # (.semgrep.yml header → --exclude-rule,
│   │                         #  pyproject.toml S-codes → bandit --skip)
│   ├── featurecoverage.h/.cpp # Feature-coverage audit lanes: spec↔code drift
│   │                         # + CHANGELOG↔feature-test coverage (in-process
│   │                         # runners, exercised by tests/features/feature_coverage)
│   ├── xcbpositiontracker.h/.cpp # KWin window position tracker
│   ├── claudeintegration.h/.cpp  # Claude Code process detection + hooks
│   ├── claudeallowlist.h/.cpp    # Claude Code permission rule editor
│   ├── claudeprojects.h/.cpp     # Claude Code project/session browser
│   ├── claudetranscript.h/.cpp   # Claude Code transcript viewer
│   ├── luaengine.h/.cpp      # Lua 5.4 scripting engine (sandboxed)
│   └── pluginmanager.h/.cpp  # Plugin discovery, loading, lifecycle
├── assets/                   # Icons
├── tests/
│   ├── audit_self_test.sh    # CTest regression driver for audit rule regexes
│   └── audit_fixtures/       # Per-rule bad.*/good.* pairs with `// @expect <rule-id>`
├── CHANGELOG.md              # Version history (Keep a Changelog format)
├── ROADMAP.md                # Release-plan for 0.6 through 1.0 and beyond
├── PLUGINS.md                # Plugin authoring guide (source of truth for API)
├── STANDARDS.md
├── RULES.md
└── README.md
```

## Architecture

Data flows: **PTY -> VtParser -> TerminalGrid -> TerminalWidget (QPainter or GlRenderer)**
Reverse flow: **TerminalGrid -> ResponseCallback -> PTY** (for DA, CPR, DSR)

- `Pty`: forkpty() spawns bash, QSocketNotifier for non-blocking reads
- `VtParser`: state machine decodes UTF-8, emits VtAction structs (Print, Execute, CSI, ESC, OSC, DCS, APC)
- `TerminalGrid`: processes VtActions, maintains cell grid + scrollback + cursor + modes
  - Mouse/focus/sync modes, OSC 8 hyperlinks, OSC 52 clipboard, OSC 133 shell integration
  - Kitty keyboard protocol (progressive enhancement with push/pop stack)
  - Desktop notifications (OSC 9/777), color palette notifications (CSI ? 2031)
  - OSC 9;4 progress reporting (ConEmu / MS Terminal) — disambiguated from OSC 9 notification by first-byte peek (`4;` → progress, else → notify body)
  - Combining chars stored in per-line side table (`TermLine::combining`)
  - Response callback for DA1/DA2/CPR/DSR sequences
  - Sixel graphics (DCS), Kitty graphics (APC), iTerm2 images (OSC 1337)
  - Line reflow on resize (soft-wrapped lines re-wrapped to new width)
- `TerminalWidget` (inherits QOpenGLWidget): renders grid, handles keyboard/mouse -> PTY writes
  - Two render paths: QPainter (CPU) with QTextLayout ligatures, or GlRenderer (GPU)
  - SGR mouse reporting, focus reporting, synchronized output
  - Undercurl + colored underlines, font fallback, image paste auto-save
  - Per-pixel background alpha transparency
- `GlRenderer`: glyph atlas texture (2048x2048), GLSL 3.3 shaders, instanced quad rendering
- `MainWindow`: wires everything, manages themes/config/menus/keybindings/dialogs
- `AiDialog`: chat UI for OpenAI-compatible LLM API with streaming SSE
- `SshDialog`: SSH connection bookmark manager, connects via PTY ssh command
- `LuaEngine`: embedded Lua 5.4 with sandboxed `ants` API and event handlers
- `PluginManager`: discovers plugins in ~/.config/ants-terminal/plugins/, loads init.lua
- `SessionManager`: serializes scrollback + screen to compressed binary, saves/restores
- `AuditDialog`: project-level static analysis panel. Runs a catalogue of checks
  (grep / find / cppcheck / clang-tidy / clazy-standalone / semgrep / ecosystem
  linters) through a shared pipeline: `OutputFilter` → `parseFindings` → dedup
  (SHA-256) → generated-file skip → path-rule severity shifts → inline
  suppression scan → `.audit_suppress` JSONL suppress → baseline diff → trend
  snapshot → enrichment (snippet ±3 / git blame / confidence 0-100) → render
  + SARIF v2.1.0 (with `contextRegion` + `properties.blame`) / single-file HTML
  export. Recent-changes scope (file-level and diff-line-level) and user rule
  packs via `<project>/audit_rules.json` — now including `path_rules` for
  glob-based severity shifts / skips. Inline suppressions honor both ants-
  native `// ants-audit: disable[=rule]` and foreign markers (`NOLINT`,
  `cppcheck-suppress`, `noqa`, `nosec`, `nosemgrep`, `#gitleaks:allow`,
  `eslint-disable-*`, `pylint: disable`). External-tool calibration chain
  (`audithygiene.cpp`) splices project-local suppression lists into the
  scanner invocations: `.semgrep.yml` header block `# Excluded upstream
  rules` → `--exclude-rule` flags; `pyproject.toml [tool.ruff.lint.ignore]`
  S-codes → `--skip B<nnn>` for bandit; mypy "Library stubs not installed"
  repeats collapse into one Info hint. Custom grep-rule findings can be
  post-filtered by `<project>/.audit_allowlist.json` entries
  `{rule, path_glob, line_regex, reason}`. Per-finding "🧠 Triage with AI"
  POSTs rule + snippet + blame to the configured OpenAI-compatible endpoint
  and displays `{verdict, confidence, reasoning}` inline. Multi-tool
  correlation tags cross-validated findings (★ in UI); confidence score
  (0-100) replaces the binary flag for sort and filter. Feature-coverage
  lanes (`featurecoverage.cpp`) run in-process — `inProcessRunner` on
  `AuditCheck` sidesteps QProcess so C++ logic plugs into the same
  `parseFindings → suppress → render` pipeline. Three lanes:
  `spec_code_drift` flags tests/features/*/spec.md backtick-fenced
  identifier tokens that no longer appear anywhere in the project tree
  (minus build/.git/vendor); it normalises scoped names by falling back
  to the tail component (`Class::method` → `method`) and skips Qt API /
  lint-code / placeholder pseudo-tokens. `changelog_test_coverage`
  fuzz-matches the top `##` version section's Added/Fixed bullets against
  feature-spec titles (backtick-token match wins; ≥2 significant-word
  overlap falls back). `test_health` greps tests/ subtrees for skipped /
  disabled / xfail / `.only` markers across GTEST_SKIP / QSKIP /
  pytest.mark.{skip,xfail} / unittest.skip / @Disabled / @Ignore / Jest
  `it.only` etc.

## Building

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
./ants-terminal
```

### Optional Dependencies

- **Lua 5.4** (`lua54-devel`): enables plugin system. Without it, plugins are disabled at compile time.
- **clazy** (`zypper in clazy` on Tumbleweed): enables the Qt-aware AST check
  in the Project Audit dialog. Requires a populated `build*/compile_commands.json`
  (CMake emits one with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, which we default on).
  Absent = the check self-disables; no error.
- **semgrep** (`pip install semgrep` or `zypper in semgrep`): enables the
  Semgrep structural-pattern lane alongside clazy. Uses `p/security-audit` +
  language packs (`p/c`, `p/cpp`, `p/python`, `p/javascript`). Auto-includes
  a project-local `.semgrep.yml` if present. Missing = silent no-op.
- **osv-scanner** (`go install github.com/google/osv-scanner/cmd/osv-scanner@latest`):
  multi-ecosystem CVE lane — replaces npm-audit / cargo-audit / pip-audit
  with one binary reading OSV.dev. Apache-2.0. Self-disables if missing.
- **trufflehog** (`go install github.com/trufflesecurity/trufflehog/v3@latest`):
  verified secret scanning — hits the API to confirm live credentials
  (default lane only reports verified hits, avoiding gitleaks's regex
  noise). AGPL-3.0, shell-out only so no license reach. Default OFF
  (needs user opt-in since verification makes network calls).
- **hadolint** (`zypper in hadolint` on Tumbleweed or static binary):
  Dockerfile linter with embedded shellcheck. Auto-detected on any
  `Dockerfile*` in the tree. GPL-3.0, shell-out only.
- **checkov** (`pip install checkov`): Infrastructure-as-Code scanner —
  Terraform, Kubernetes, GitHub Actions, Dockerfile. Auto-detected when
  any `*.tf`, `.github/workflows/`, or `k8s/` tree is present. Apache-2.0.
- **ast-grep** (`cargo install ast-grep` or `npm i -g @ast-grep/cli`):
  polyglot Tree-sitter structural search. Rule-pack-driven — gated on a
  `sgconfig.yml` at project root. MIT-licensed.
- **cppcheck / clang-tidy / shellcheck / pylint / bandit / ruff / …**: each
  audit check probes with `which <tool>` and self-disables if missing.

### Testing

```bash
cd build && ctest --output-on-failure
```

Two test lanes:

**`audit_rule_fixtures`** — runs `tests/audit_self_test.sh` to verify
audit rule regexes match their `bad.*` fixture files and none of their
`good.*` counterparts. Count-based assertion (not line numbers — more
robust to fixture editing). To add a rule test:

1. `mkdir tests/audit_fixtures/<rule-id>/`
2. Drop a `bad.cpp` with N `// @expect <rule-id>` markers on lines that
   should match, plus a `good.cpp` with zero matches.
3. Add `run_rule "<rule-id>" '<regex>'` to `tests/audit_self_test.sh`
   (regex must mirror the `addGrepCheck()` call in `auditdialog.cpp`).

**Feature-conformance (`tests/features/*`, label `features`)** — each
subdirectory pairs a `spec.md` (human contract) with a C++ executable
that exercises the feature through its public API and asserts the
contract holds. Compiled as standalone CTest targets linking only the
`src/*.cpp` objects each test actually exercises (GUI-free, fast).
Purpose: catch *behavioral* regressions unit tests miss — e.g. the
0.6.22 scrollback fix would have caught the incomplete 0.6.21 fix at
commit time, because the 0.6.21 fix passed unit tests but failed the
"scrollback must not double on CSI 2J repaint" invariant. To add a
feature test:

1. `mkdir tests/features/<feature-name>/`
2. Write `spec.md` — invariants in plain English, reviewable by humans.
3. Write `test_<feature>.cpp` — exit 0 on pass, non-zero on fail, print
   enough context on failure to diagnose without reproducing.
4. Wire into `CMakeLists.txt` as `add_executable` + `add_test` with
   label `features;fast`.
5. Run the test against the *pre-fix* code (via `git checkout <prev-sha>
   -- src/...`) and confirm it fails — prevents writing a test that
   passes on broken code. Restore the fix afterwards.

See `tests/features/README.md` for the full authoring guide.

### Static Analysis

Cppcheck requires `--library=qt` for this codebase. Without it, cppcheck misparses the Qt `emit` macro as a type and reports every signal emission as a local-variable shadow. The correct invocation:

```bash
cppcheck --enable=all --std=c++20 --library=qt \
         --suppress=missingIncludeSystem --suppress=unusedFunction \
         --suppress=unknownMacro -I src src/
```

## Conventions

- Signals/slots for cross-component communication
- Theme colors set on TerminalGrid (default fg/bg), used during rendering
- ANSI palette (16 standard + 216 cube + 24 gray) initialized in TerminalGrid
- Config persists to ~/.config/ants-terminal/config.json with 0600 permissions
- Scrollback configurable up to 1,000,000 lines (default 50,000)
- QTextLayout used for text rendering (enables ligature shaping via HarfBuzz)
- Lua plugins optional: gated behind `ANTS_LUA_PLUGINS` compile definition

## Key Design Decisions

- Custom VT100 parser (not pyte/libvterm) -- no external deps beyond Qt6
- QOpenGLWidget base class: GPU-accelerated QPainter by default, optional custom GL renderer
- QTextLayout for ligature-aware text shaping (HarfBuzz backend)
- Alt screen buffer support (for vim, htop, etc.)
- Delayed wrap (like xterm) for correct line-wrapping behavior
- SGR mouse reporting, focus reporting, synchronized output for TUI app compatibility
- Combining chars in per-line side table (zero memory overhead for lines without combiners)
- Image paste auto-saves to disk and inserts filepath (optimized for Claude Code workflow)
- Keybindings configurable via config.json
- GPU rendering via glyph atlas + GLSL shaders as optional accelerated path
- AI integration uses OpenAI-compatible API (works with Ollama, LM Studio, OpenAI, etc.)
- SSH manager uses system `ssh` binary via PTY (inherits user's SSH config/keys)
- Lua plugin sandbox removes dangerous globals, adds instruction count timeout
- Session persistence uses QDataStream + qCompress for efficient binary serialization
- Per-pixel transparency separate from window opacity (background alpha vs window alpha)
- Audit rule pack uses JSON (`audit_rules.json`) not YAML — QJsonDocument is
  built-in; flat schema makes YAML's readability win marginal. Hardcoded checks
  stay in C++; `audit_rules.json` only appends / overrides
- Audit uses clazy-standalone (Qt-aware AST) rather than embedding libclang —
  clazy ships every Qt check we need, reads our existing `compile_commands.json`,
  and retires several regex-based Qt checks that were FP-prone
- `.audit_suppress` is JSONL v2 (`{key, rule, reason, timestamp}` per line);
  v1 plain-key lines still load, first write converts in place
- Audit external-tool calibration reads project-local config that *already
  exists* for each scanner, rather than introducing a new suppression file
  per tool. `.semgrep.yml` header → `--exclude-rule`; `pyproject.toml`
  `[tool.ruff.lint.ignore]` S-codes → bandit `--skip B<nnn>` (ruff already
  mirrors bandit 1:1 on the S family). The project-local
  `.audit_allowlist.json` only exists for custom grep-rule findings that
  have no upstream config to read. Rationale: RetroDB 2026-04-21 audit-
  hygiene report showed 1.3% signal rate where all noise was already
  documented in existing files — the fix was teaching the runner to read
  them, not adding a new calibration file
- Audit test harness is shell-based against fixture dirs (`bad.*`/`good.*` with
  `// @expect` markers) — no C++ unit framework, no link-time coupling to
  auditdialog internals
- Audit context-awareness follows reference-tool conventions: inline
  suppression directives recognize clang-tidy (`NOLINT`/`NOLINTNEXTLINE`),
  cppcheck (`cppcheck-suppress`), flake8 (`noqa`), bandit (`nosec`), semgrep
  (`nosemgrep`), gitleaks (`#gitleaks:allow`), eslint (`eslint-disable-*`),
  pylint (`pylint: disable`) plus ants-native `// ants-audit: disable[=rule]`.
  Generated files (`moc_*`, `ui_*`, `qrc_*`, `*.pb.cc/.h`, `/generated/`,
  `_generated.*`) are auto-skipped. SARIF exports include `contextRegion`
  (±3 lines around the finding) and a `properties.blame` bag following the
  sarif-tools de-facto convention
- Confidence score (0-100) replaces the binary `highConfidence` flag: base
  = severity×15, +20 cross-tool, +10 external AST tool, −20 test path,
  adjusted by explicit AI-triage verdicts. Sorted-by-confidence view
  surfaces the most trustworthy findings first
- Versioning follows [SemVer](https://semver.org). `project(... VERSION X.Y.Z)`
  in `CMakeLists.txt` is the single source of truth; `add_compile_definitions(
  ANTS_VERSION="${PROJECT_VERSION}")` propagates it to `main.cpp`
  (setApplicationVersion), `auditdialog.cpp` (SARIF driver, dialog title,
  types-label badge, HTML meta, plain-text handoff header). Bump in
  `CMakeLists.txt` and every caller picks it up on rebuild — never hardcode
  version strings in `.cpp`/`.h` files
- Release notes live in `CHANGELOG.md` at project root following
  [Keep a Changelog](https://keepachangelog.com) format. Every bump touches
  three files: `CMakeLists.txt` (VERSION), `CHANGELOG.md` (new dated
  section), and `README.md` (the "Current version" line)
- `ROADMAP.md` is the forward-looking record — release-arc themes (0.6
  polish, 0.7 shell integration, 0.8 multiplexing/marketplace, 0.9
  platform/a11y, 1.0 stability) with prior-art links. When you complete
  an item, move it from ROADMAP.md (status 📋) into the matching
  CHANGELOG.md section (✅ Done) for the release it shipped in
- `PLUGINS.md` is the plugin-author contract — when `luaengine.cpp` /
  `pluginmanager.cpp` change what the `ants.*` API exposes, update
  PLUGINS.md in the same commit. The doc is the contract; CI doesn't
  check drift yet (honor system)

## Config Keys

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `theme` | string | `"Dark"` | Active theme name |
| `font_size` | int | `11` | Font size (8-32) |
| `opacity` | double | `1.0` | Window opacity (0.1-1.0) |
| `background_alpha` | int | `255` | Per-pixel bg alpha (0-255) |
| `scrollback_lines` | int | `50000` | Max scrollback (1K-1M) |
| `gpu_rendering` | bool | `false` | Use OpenGL glyph atlas renderer |
| `session_persistence` | bool | `false` | Save/restore scrollback on exit |
| `ai_endpoint` | string | `""` | OpenAI-compatible API URL |
| `ai_api_key` | string | `""` | API authentication key |
| `ai_model` | string | `"llama3"` | LLM model name |
| `ai_context_lines` | int | `50` | Terminal lines sent as context |
| `ai_enabled` | bool | `false` | Enable AI features |
| `ssh_bookmarks` | array | `[]` | Saved SSH connections |
| `plugin_dir` | string | `""` | Lua plugin directory |
| `enabled_plugins` | array | `[]` | Active plugin names |
| `snippets` | array | `[]` | Command snippets with placeholders |
| `auto_profile_rules` | array | `[]` | Auto-switch profiles by pattern |
| `badge_text` | string | `""` | Watermark text in terminal bg |
| `auto_color_scheme` | bool | `false` | Auto-switch dark/light theme |
| `dark_theme` | string | `"Dark"` | Theme for dark system mode |
| `light_theme` | string | `"Light"` | Theme for light system mode |
