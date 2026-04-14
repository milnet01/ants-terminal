# Changelog

All notable changes to Ants Terminal are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Sections use the standard categories — **Added** for new features, **Changed**
for changes in existing behavior, **Deprecated** for soon-to-be-removed features,
**Removed** for now-removed features, **Fixed** for bug fixes, and **Security**
for security-relevant changes.

## [Unreleased]

Nothing yet — items queued for 0.7 live in [ROADMAP.md](ROADMAP.md).

## [0.6.2] — 2026-04-14

**Theme:** Claude Code status readability. Adds a dedicated "compacting" state
so the status bar doesn't flatten a multi-second `/compact` into generic
"thinking…".

### Added

- **"Claude: compacting…" status.** New `ClaudeState::Compacting` surfaces
  when a `/compact` is in flight, rendered in magenta so it's visually
  distinct from idle (green), thinking (blue), and tool use (yellow).
  Detection is transcript-driven: the parser walks the existing 32KB tail
  window looking for a `user` event whose string content contains
  `<command-name>/compact</command-name>`, and only fires while no
  subsequent `isCompactSummary:true` user event (the condensed-history
  marker Claude Code writes when compaction finishes) has appeared. The
  `PreCompact` hook path now routes through the same state, so terminals
  that have the hook server wired up get the same indicator without the
  2-second poll latency.

## [0.6.1] — 2026-04-14

**Theme:** scroll-anchor correctness. One-liner fix for a long-standing drift
bug that only surfaced with a full scrollback buffer.

### Fixed

- **Scroll anchor drifts when scrollback is at its cap.** While scrolled up
  reading history, bursts of new output (e.g. Claude Code redrawing its
  prompt / spinner) appeared to "redraw text at the current history
  position" — actually the pinned viewport was sliding by one content-line
  per pushed line. Root cause: the anchor math in `onPtyData` diffed
  `scrollbackSize()` before/after parsing, but once the buffer hits
  `scrollback_lines` (default 50k) each push also pops a stale line from
  the front, so the size delta is always zero and the anchor thinks
  nothing happened. `TerminalGrid` now exposes a monotonic
  `scrollbackPushed()` counter that keeps incrementing through the cap;
  `TerminalWidget` diffs that instead. Below-cap behavior is unchanged.

## [0.6.0] — 2026-04-14

**Theme:** make the two features that already make Ants distinctive
(plugins + audit) production-grade. Ships the full context polish + plugin v2
arc from the 0.6 roadmap — scrollback regex search, OSC 9;4 progress,
multi-line paste confirmation, OSC 8 homograph warning, LRU glyph-atlas
eviction, cell-level dirty tracking, per-plugin Lua VMs, manifest v2 with
declarative permissions + capability-gated APIs, plugin keybindings, and
hot reload.

### Added

#### 🎨 Features

- **Scrollback regex search**. Ctrl+Shift+F search bar gains three toggle
  buttons: `.*` (regex mode via `QRegularExpression`), `Aa` (match case),
  and `Ab` (whole word, wraps the pattern in `\b…\b`). Alt+R / Alt+C /
  Alt+W flip toggles without leaving the input. Invalid regex patterns
  render the input with a red border and show the error in the match-
  counter tooltip (`!/!`). Zero-width matches (`\b`, `^`) are detected
  and skipped to avoid infinite loops. Matches Kitty / iTerm2 / WezTerm
  behavior; closes the Ghostty 1.3 gap.
- **OSC 9;4 progress reporting**. Parses ConEmu / Microsoft Terminal
  `ESC]9;4;state;percent ST` sequences. State 0 clears, 1 shows a normal
  blue progress bar, 2 shows a red error bar, 3 shows an indeterminate
  (full-width) lavender bar, 4 shows a yellow warning bar. Renders as a
  3-pixel strip along the bottom edge of the terminal (above the
  scrollbar) and as a small colored dot next to the tab title. Coexists
  with the existing OSC 9 desktop-notification handler via payload
  disambiguation (`9;4;…` vs. `9;<body>`). Spec:
  [MS Terminal progress sequences](https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences).
- **Click-to-move-cursor on shell prompts** (previously in code, now
  documented). When the cursor is in an OSC 133 prompt region with no
  output yet, clicking on the same line sends the appropriate run of
  `ESC [ C` / `ESC [ D` arrow sequences to reposition the cursor. Capped
  at 200 arrows to guard against runaway sends.

#### ⚡ Performance

- **LRU glyph-atlas eviction**. Replaces the old "clear everything on
  overflow" behavior with a warm-half retention policy: entries touched
  within the last 300 frames are kept, colder ones are re-rasterized
  into a fresh atlas. Median-based fallback handles cold-boot overflow.
  Eliminates the paint stall on long ligature-heavy sessions. See
  `glrenderer.cpp:compactAtlas`.
- **Per-line dirty tracking**. `TermLine` gains a `dirty` flag set by
  every grid mutation (print, erase, scroll, alt-screen swap, resize).
  `TerminalWidget::invalidateSpanCaches()` now does targeted eviction —
  only URL / highlight caches for dirty lines get dropped, not the whole
  map. Big win on high-throughput output that only touches a few lines.
  (Full cell-level render-path partial-update is deferred; the dirty
  primitive is in place for that work.)

#### 🔒 Security

- **Multi-line paste confirmation**. Dialog appears when the pasted
  payload contains a newline, the string `sudo `, a curl/wget/fetch
  piped into `sh`/`bash`/`python`/etc., or any non-TAB/LF/CR control
  character. Policy is independent of bracketed paste — iTerm2 /
  Microsoft Terminal got bitten by conflating the two
  ([MS Terminal #13014](https://github.com/microsoft/terminal/issues/13014)).
  Config key: `confirm_multiline_paste` (default `true`). UI:
  Settings → Behavior → "Confirm multi-line / dangerous pastes".
- **Per-terminal OSC 52 write quota**. 32 writes/min + 1 MB/min per
  grid, independent of the existing 1 MB per-string cap. Protects
  against drip-feed exfiltration.
- **OSC 8 homograph warning**. When an OSC 8 hyperlink's visible label
  encodes a hostname-looking token (`github.com`) that doesn't match
  the actual URL host (`github.evil.example.com`), a confirm dialog
  shows both and requires explicit opt-in. Strips leading `www.` for
  fair comparison.

#### 🔌 Plugins — manifest v2 + capability model

- **Per-plugin Lua VMs**. Each plugin gets its own `lua_State`, heap
  budget (10 MB), and instruction hook (10 M ops). One VM stalling or
  leaking globals can no longer affect others. `PluginManager` owns
  a map of `(name → LuaEngine *)` and fans out events to each in turn.
- **Declarative permissions in `manifest.json`**. Plugins declare a
  `"permissions": ["clipboard.write", "settings"]` array. On first
  load, a permission dialog lists each requested capability and lets
  the user accept / un-check individual grants. Grants persist in
  `config.json` under `plugin_grants`; subsequent loads don't re-prompt
  unless the manifest requests a new permission. Un-granted permissions
  surface as *missing API functions* (not `nil` stubs) so plugins can
  feature-detect with `if ants.clipboard then … end`.
- **`ants.clipboard.write(text)`** — gated by `"clipboard.write"`
  permission. Writes to the system clipboard via QApplication.
- **`ants.settings.get(key)` / `ants.settings.set(key, value)`** — gated
  by `"settings"` permission. Per-plugin key/value store, persisted
  under `plugin_settings.<plugin_name>.<key>` in `config.json`. Manifest
  can declare a `"settings_schema"` JSON-Schema subset (plumbing in
  place; schema-driven Settings UI is a follow-up item).
- **Plugin keybinding registration** via manifest `"keybindings"` block:
  `{"my-action": "Ctrl+Shift+K"}`. Firing the shortcut dispatches a
  `keybinding` event to the owning plugin with the action id as the
  payload. `ants.on("keybinding", function(id) ... end)` listens. The
  shortcut lives on the MainWindow; invalid sequences log a warning.
- **Plugin hot reload** via `QFileSystemWatcher`. Watches `init.lua`,
  `manifest.json`, and the plugin directory itself; on change, debounced
  150 ms, the plugin VM is torn down (fires `unload` event) and
  re-initialized (fires `load` event). Enabled only when
  `ANTS_PLUGIN_DEV=1` — idle cost is zero otherwise.
- **`ants._version`** exposes `ANTS_VERSION` string so plugins can
  feature-detect without hardcoding.
- **`ants._plugin_name`** exposes the plugin's declared manifest name.
- **New events**: `load`, `unload` (lifecycle), `keybinding` (manifest
  shortcuts). Added to `eventFromString()` in `luaengine.cpp`.

#### 🧰 Dev experience

- **`ants-terminal --new-plugin <name>` CLI**. Creates a plugin skeleton
  at `~/.config/ants-terminal/plugins/<name>/` with `init.lua`,
  `manifest.json` (empty `permissions` array), and `README.md` templates.
  Validates the name against `[A-Za-z][A-Za-z0-9_-]{0,63}`.
- **`ANTS_PLUGIN_DEV=1`** env var enables verbose plugin logging on
  scan/load, plus hot reload. Cached per-process via a static.
- **`--help` / `--version`** flags wired through `QCommandLineParser`
  (previously `--version` set via `setApplicationVersion` but no CLI
  handler existed).

### Changed

- `README.md` — navbar now links to `PLUGINS.md`, `ROADMAP.md`,
  and `CHANGELOG.md`. Plugins section points to PLUGINS.md as the
  source of truth; keeps a quick-start summary table inline.
- `CLAUDE.md`, `STANDARDS.md`, `RULES.md` — cross-linked to the new
  docs; plugin-system sections defer to PLUGINS.md for the author
  contract and document internal invariants separately.
- `PluginManager` no longer exposes a single `engine()` accessor;
  callers use `engineFor(name)` to get the per-plugin VM. Event
  delivery fans out over all loaded engines.
- Paste path now runs through `TerminalWidget::confirmDangerousPaste`
  before the bracketed-paste wrap — confirmation policy is
  independent of PTY-side behavior.

## [0.5.0] — 2026-04-13

### Added

**Project Audit — context-aware upgrade** (this release's headline feature).
The audit pipeline now collects and applies context at every stage, closing
the gap with commercial tools (CodeQL, Semgrep, SonarQube, DeepSource).

- **Inline suppression directives.** Findings can be silenced directly in
  source via `// ants-audit: disable[=rule-id]` (same line),
  `disable-next-line`, and `disable-file` (first 20 lines). Foreign markers
  are also honored for cross-tool compatibility: clang-tidy `NOLINT` /
  `NOLINTNEXTLINE`, cppcheck `cppcheck-suppress`, flake8 `noqa`, bandit
  `nosec`, semgrep `nosemgrep`, gitleaks `#gitleaks:allow`, eslint
  `eslint-disable-*`, pylint `pylint: disable`. Rule-list parsing and
  glob matching (`google-*`) are supported.
- **Generated-file auto-skip.** Findings in `moc_*`, `ui_*`, `qrc_*`,
  `*.pb.cc`/`*.pb.h`, `*.grpc.pb.*`, flex/bison outputs, `/generated/`
  paths, and `*_generated.*` files are dropped before rendering.
- **Path-based rules** in `audit_rules.json`. A new `path_rules[]` block
  takes entries of `{glob, skip, severity_shift, skip_rules[]}` to
  configure per-directory severity shifts and skips (e.g. `tests/**`
  shifts everything down one severity band; `third_party/**` skips
  entirely). Glob syntax supports `**`, `*`, `?`.
- **Code snippets** (±3 lines) attached to every `file:line` finding,
  cached per-file, rendered with the finding line highlighted. Exposed
  via SARIF `physicalLocation.contextRegion`, embedded in the HTML
  report's expandable `[details]` panel, and included in the Claude
  review handoff.
- **Git blame enrichment.** Author, author date, and short SHA shown
  next to every finding. Cached per `(file, line)` with a 2 s timeout;
  auto-disabled for non-git projects. Exported via SARIF
  `properties.blame` bag (sarif-tools de-facto convention).
- **Confidence score 0-100.** Replaces the binary `highConfidence` flag
  with a weighted sum: severity × 15 + 20 cross-tool agreement + 10
  external AST tool − 20 test-path, clamped, adjusted by explicit AI
  verdicts. Displayed as a coloured pip in the UI; exported in SARIF
  (`properties.confidence`) and HTML.
- **Severity pill filter + live text filter** in the dialog, matching the
  HTML export's UX. Filters across file name, message, rule id, and
  author (via blame). Re-renders instantly.
- **Sort by confidence** toggle reorders every check's findings by
  confidence descending — the "which should I look at first" view.
- **Collapsible `[details]`** per finding. Click to reveal snippet
  context and an inline "🧠 Triage with AI" action. Expanded state
  persists across re-renders.
- **Semgrep** integration (optional). When `semgrep` is available, a
  Security-category check runs `p/security-audit` plus language packs
  (`p/c`, `p/cpp`, `p/python`, `p/javascript`, `p/typescript`). Picks
  up project-local `.semgrep.yml` automatically. Complements clazy's
  Qt-aware AST lane.
- **AI triage per finding.** Click "🧠 Triage with AI" inside an
  expanded finding to POST rule + message + snippet + blame to the
  project's configured OpenAI-compatible endpoint. Response parsed
  as `{verdict, confidence, reasoning}`; verdict badge rendered
  inline, confidence score adjusted accordingly. Opt-in per click,
  respects `Config::aiEnabled`.
- **Version stamp on all audit outputs.** Dialog title, dialog types
  label, SARIF `driver.version`, HTML report meta line, and plain-text
  Claude handoff now all carry `ants-audit v<PROJECT_VERSION>` pulled
  from a single CMake `add_compile_definitions(ANTS_VERSION=…)` source
  of truth.
- **Single-source-of-truth versioning infrastructure.**
  `CMakeLists.txt` `project(... VERSION 0.5.0)` propagates via
  `add_compile_definitions(ANTS_VERSION=…)` to every caller. Four
  previously-hardcoded `"0.4.0"` literals retired: `main.cpp`
  (`setApplicationVersion`), `auditdialog.cpp` (SARIF driver),
  `ptyhandler.cpp` (`TERM_PROGRAM_VERSION` env), and
  `claudeintegration.cpp` (MCP `serverInfo.version`).

### Changed

- **`audit_rules.json` schema** extended with `path_rules[]`. Existing
  `rules[]` entries load unchanged; loader documents both blocks.
- **SARIF export** now emits `physicalLocation.contextRegion` with the
  full ±3 snippet for every finding that has a `file:line` location,
  plus `properties.blame` and `properties.confidence`. `properties`
  also carries `aiTriage` when the user triaged that finding.
- **HTML report** template upgraded with confidence pips, blame tags,
  verdict badges, and per-finding expandable snippet panels. Added
  CSS classes (`.conf-pip`, `.blame`, `.verdict`, `.snippet`, `.hit`).
  `DATA.version` now propagated through the inline payload.
- **Plain-text Claude handoff** body lines carry `[conf N · blame · AI]`
  tags and a 3-line indented snippet per finding. Header gains a
  `Generator:` line.
- **`dropFindingsInCommentsOrStrings`** still opt-in per check via the
  existing `kSourceScannedChecks` set, but is now augmented by the
  inline-suppression scan (applied to *every* finding with a
  `file:line`). Pipeline order documented in `STANDARDS.md`.

### Security

- Inline suppression scan reads files through a bounded per-file cache
  (4 MB cap) — runaway check on a huge file cannot stall the dialog.
- Git blame shells out with a 2 s timeout and is auto-disabled when
  not in a git repository.
- AI triage respects the project's existing `ai_enabled` config and
  refuses non-http/https endpoints.

### Tests / Tooling

- `tests/audit_self_test.sh` extended with 18 suppression-token
  assertions covering every honored marker (ants-native,
  NOLINT/NOLINTNEXTLINE, cppcheck-suppress, noqa, nosec, nosemgrep,
  gitleaks-allow, eslint-disable-*, pylint: disable) plus negative
  cases. Total: 23 rule tests passing.
- `ANTS_VERSION` compile definition wired via CMake
  `add_compile_definitions`. `main.cpp` and `auditdialog.cpp` no
  longer carry hardcoded version strings.

### Docs

- `README.md` — expanded "Project Audit Dialog" section with examples
  of inline suppression directives and `audit_rules.json` schema
  including `path_rules`; semgrep added to optional dependencies.
- `CLAUDE.md` — pipeline order, confidence formula, generated-file
  patterns, new design decisions around context-awareness.
- `STANDARDS.md` — context-awareness pipeline order invariants, blame
  cache rules, AI triage config sourcing.
- `RULES.md` — three new audit rules covering inline-suppression
  preference, `path_rules` vs. hardcoded paths, and "confidence is
  display-only, not a gate".

## [0.4.0] — 2026-04-13

### Added

**Project Audit dialog — full feature arc.** Over the course of eight
incremental commits, the audit panel evolved from a regex-heavy scanner
into a structured, AST-aware, cross-validated tool.

- **SonarQube-style taxonomy** — every finding carries a type
  (Info / CodeSmell / Bug / Hotspot / Vulnerability) and a 5-level
  severity (Info / Minor / Major / Critical / Blocker).
- **Per-finding dedup keys** (SHA-256 of `file:line:checkId:title`).
- **Baseline diff** — save current findings to
  `.audit_cache/baseline.json`; later runs highlight only new findings.
- **Trend tracking** — severity counts persisted at
  `.audit_cache/trend.json` (last 50 runs) and rendered as a delta
  banner against the previous run.
- **Recent-changes scope** — restrict findings to files touched in
  the last N commits, or (stricter) to exact diff hunk line ranges
  via `git diff --unified=0 HEAD~N`.
- **Multi-tool correlation** — ★ badge on findings flagged at the
  same `file:line` by ≥2 distinct tools.
- **clazy integration** — Qt-aware AST checks (`connect-3arg-lambda`,
  `container-inside-loop`, `old-style-connect`, `qt-keywords`, etc.)
  via `clazy-standalone`, reading the project's
  `compile_commands.json`. Retires three FP-prone regex checks.
- **Comment/string-aware filtering** — per-check opt-in state-machine
  scan that drops matches inside `//`, `/* */`, or `"strings"`.
- **SARIF v2.1.0 export** — OASIS-standard JSON consumed by GitHub
  Code Scanning, VSCode SARIF Viewer, SonarQube, CodeQL. Includes
  per-rule catalogue, `partialFingerprints`, and per-finding
  properties bag.
- **Single-file HTML report** — no external assets, embedded JSON
  payload + vanilla JS. Severity pills, text filter, collapsible
  check cards.
- **Interactive suppression** — click any dedup hash to prompt for
  a reason and append to `.audit_suppress` (JSONL v2: `{key, rule,
  reason, timestamp}` per line).
- **User-defined rules** via `<project>/audit_rules.json`. Flat
  schema mirrors the internal `AuditCheck` struct. User rules run
  through the full filter / parse / dedup / suppress pipeline.
- **CTest harness** — `tests/audit_self_test.sh` with per-rule
  `bad.*`/`good.*` fixtures using `// @expect <rule-id>` markers;
  count-based assertion.
- **Review with Claude** handoff — emits a plain-text report with
  `CLAUDE.md`, `STANDARDS.md`, `RULES.md`, and `CONTRIBUTING.md`
  prepended so Claude can weigh findings against documented rules.

### Changed

- `.audit_suppress` upgraded from v1 plain-key-per-line to v2 JSONL;
  v1 still loads, first write converts in place with a migration
  marker.
- Audit rule pack format: JSON (`audit_rules.json`), not YAML —
  `QJsonDocument` is built-in; flat schema's readability gap with
  YAML is small enough that a parser dependency isn't worth it.

### Security

- `audit_rules.json` is a trust boundary — its `command` field runs
  through `/bin/bash` unconditionally. Documented analogous to
  `.git/hooks`: your repo, your commands, no sandbox.

## [0.3.0]

### Added

- Claude Code deep integration: live status bar, project/session
  browser (Ctrl+Shift+J), permission allowlist editor (Ctrl+Shift+L),
  transcript viewer, slash-command shortcuts.
- 12 professional UX features: hot-reload config, column selection,
  sticky headers, tab renaming/coloring, snippets, auto-profile
  switching, badge watermarks, dark/light auto-switching, Nerd Font
  fallback, scrollbar, Ctrl+arrow word movement, scroll anchoring.
- Session persistence: save/restore scrollback via `QDataStream` +
  `qCompress` binary serialization.
- AI assistant dialog: OpenAI-compatible chat completions with SSE
  streaming, configurable endpoint/model.
- SSH manager: bookmark editor, PTY-based `ssh` connection.
- Lua 5.4 plugin system: sandboxed `ants` API, instruction-count
  timeout, event-driven handlers.
- GPU rendering path: QOpenGLWidget with glyph atlas, GLSL 3.3
  shaders.

### Fixed

- Six rounds of comprehensive security + correctness audits covering
  PTY FD leaks, bracketed-paste injection, SIGPIPE, Lua coroutine
  escape, hardcoded colours, atomic config writes, and more.

## [0.2.0]

### Added

- Full C++ terminal emulator rewrite: custom VT100/xterm state-machine
  parser, `TerminalGrid` with scrollback + alt screen, PTY via
  `forkpty`, QPainter rendering with `QTextLayout` ligature shaping.
- Mouse reporting (SGR format), focus reporting (mode 1004),
  synchronized output (mode 2026).
- OSC 8 hyperlinks, OSC 52 clipboard write, OSC 133 shell-integration
  markers, OSC 9 / 777 desktop notifications.
- Sixel graphics (DCS), Kitty graphics protocol (APC), iTerm2 images
  (OSC 1337).
- Kitty keyboard protocol with progressive enhancement (push / pop /
  query / set).
- Combining characters via per-line side table.
- Command palette (Ctrl+Shift+P), tab management with custom frameless
  titlebar, split panes via nested `QSplitter`s, 11 built-in themes.

## [0.1.0]

### Added

- Initial release: basic terminal emulator prototype.
