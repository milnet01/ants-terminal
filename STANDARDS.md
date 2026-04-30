# Development Standards

## Code Style

- **C++20** standard required
- **Qt6** naming conventions for Qt-derived classes
- `m_` prefix for member variables (e.g., `m_cursorRow`, `m_scrollOffset`)
- `s_` prefix for static member variables (e.g., `s_palette256`)
- Include guards via `#pragma once`
- Braces on same line for function/control bodies (K&R style)

## Naming

- `PascalCase` for classes and structs (`TerminalGrid`, `CellAttrs`, `TermLine`)
- `camelCase` for methods, functions, and local variables (`handlePrint`, `scrollbackSize`)
- `UPPER_SNAKE_CASE` for compile-time constants and macros
- `camelCase` for signal and slot names (`titleChanged`, `shellExited`, `onPtyData`)
- File names match the primary class in lowercase (`terminalgrid.cpp` for `TerminalGrid`)

## File Organization

- Headers: `#pragma once`, then project includes, then library includes, then system includes
- Source files: matching header first, then other project headers, then Qt headers, then system headers
- Separate include groups with blank lines
- One class per header/source pair

## Architecture

- **Signals/slots** for all cross-component communication -- no direct method calls between siblings
- **Config class** for all persistent state -- never write JSON directly
- **Themes** as the single source of truth for all colors -- no hardcoded color values in widgets
- Data flows: PTY -> VtParser -> TerminalGrid -> TerminalWidget (QPainter/OpenGL)
- Reverse data flow for DA/CPR/DSR: TerminalGrid -> ResponseCallback -> PTY
- Each class has a single clear responsibility

## Rendering Standards

- **CPU path (the only path):** QPainter with QTextLayout for
  ligature-aware text shaping. The 0.7.44 release retired the optional
  GPU/glrenderer path (`glrenderer.cpp`) — the QPainter+QTextLayout
  path is fast enough with the glyph-shape cache (ANTS-1115 perf-sweep
  row 3) and avoids the maintenance cost of a second renderer.
- Per-pixel background alpha applied in paint loop, not at window level
- Cell backgrounds drawn before text; cursor drawn last
- The chrome (title bar, menus, tabs, status bar) always paints opaque
  via `WA_StyledBackground`; only the terminal-area `fillRect` honours
  the user's `opacity` config (the prior `background_alpha` config key
  was removed as redundant in 0.7.18 — see `CLAUDE.md` Key design
  decisions).

## Plugin System Standards

The plugin-author-facing contract lives in [PLUGINS.md](PLUGINS.md); this
section captures the internal invariants the runtime must preserve to
keep that contract honest.

- Lua 5.4 embedded via C API with sandboxed environment
- Dangerous globals removed: `os`, `io`, `loadfile`, `dofile`, `load`,
  `require`, `package`, `debug`, `setmetatable`, `getmetatable`,
  `rawget`, `rawset`, `rawequal`, `rawlen`, `collectgarbage`,
  `coroutine` — keep PLUGINS.md's sandbox table in sync with any change here
- Instruction count hook for timeout protection (10M instructions max)
- Heap budget enforced by custom allocator (10 MB Lua heap cap)
- Bytecode rejection at load: first-byte check for `0x1B` refuses
  `.luac`-style inputs (prevents sandbox escapes via forged bytecode)
- Plugin events fired synchronously on Qt event loop — any plugin
  stalling its handler stalls the terminal; no worker threads
- Plugin API namespaced under `ants.*` table; adding a new function
  requires an entry in PLUGINS.md's API section and a CHANGELOG note
- Plugins stored in `~/.config/ants-terminal/plugins/<name>/init.lua`
  with optional `manifest.json`
- **Single source of truth**: `luaengine.cpp` enforces the sandbox;
  `pluginmanager.cpp` handles discovery/lifecycle; PLUGINS.md documents
  what's exposed. Drift is a bug — if you find a discrepancy, the doc
  is authoritative until a fix lands

## Network Standards

- AI backend uses OpenAI-compatible chat completions API (`/v1/chat/completions`)
- SSE streaming for progressive response display
- API keys stored in config.json (0600 permissions)
- Non-HTTPS endpoints allowed but not recommended
- Network timeout: 30 seconds
- No network connections made without explicit user action

## VT Protocol Standards

- CSI private parameter prefixes (`?`, `>`, `=`, `<`) stored in the intermediate string
- Response sequences (DA1, DA2, CPR, DSR) emitted via `ResponseCallback`, never by direct PTY writes from grid
- Mouse events reported in SGR format (`CSI < Cb;Cx;Cy M/m`) with 1-based coordinates
- Focus events sent as `CSI I` (focus in) and `CSI O` (focus out) only when mode 1004 is active
- Synchronized output (`?2026h/l`) defers widget repaints, not VT processing
- OSC 52 clipboard write allowed; clipboard read (query) disabled for security
- OSC 8 hyperlinks tracked per-line; nested links implicitly close the previous
- OSC 133 shell integration markers (A/B/C/D) stored as prompt regions
- Underline styles use colon subparameter syntax (`4:N`); both colon and semicolon forms accepted
- Combining characters stored in per-line side table, not per-cell, to minimize memory overhead
- Sixel graphics: two-pass rendering (dimension scan + pixel paint), 4096x4096 max
- Kitty graphics: chunked base64 reassembly, image cache (200 entries), PNG/RGBA/RGB formats
- Kitty keyboard protocol: progressive enhancement via CSI > u (push), CSI < u (pop), CSI ? u (query), CSI = u (set)
- Desktop notifications: OSC 9 (body-only) and OSC 777 (notify;title;body) supported
- Color palette update notifications: CSI ? 2031 h/l mode, CSI ? 996 n query, CSI ? 997 n unsolicited report

## Widget Guidelines

- Widgets emit signals; parent widgets connect them
- Widgets do not import or reference their parent by type
- Use `setObjectName()` for widgets that need specific QSS styling
- All theme-dependent colors must be set via `applyThemeColors()` or equivalent
- Keyboard shortcuts defined in `MainWindow::setupMenus()` for discoverability
- In split-pane layouts, resolve "the tab's terminal" via a helper that prefers
  the focused descendant and only falls back to first-child — `findChild<>()`
  alone returns arbitrary order and misdirects commands to the wrong pane
- When a TerminalWidget key handler consumes a key (Ctrl+Shift+Up/Down for OSC
  133 prompt nav), do NOT assign the same QKeySequence to a menu QAction — the
  shortcut will never fire from the terminal and Qt emits an ambiguity warning

## Memory Management

- Use `std::unique_ptr` for owned heap objects not managed by Qt's parent-child system
- Use Qt parent-child ownership for QObject-derived classes
- Prefer stack allocation and RAII over raw `new`/`delete`
- Use `std::move` for transferring ownership of large containers

## Error Handling

- Graceful fallbacks for missing config files (defaults applied silently)
- Never crash on user actions (paste, input, resize, theme switch)
- PTY failures reported via status bar, not crashes
- Validate all external input (OSC payloads, clipboard data, file paths)
- Network errors displayed in AI dialog, not as modal dialogs
- Lua plugin errors logged to status bar, never crash the terminal

## Performance

- Cache expensive computations in `paintEvent()` (URL detection, bracket matching)
- Avoid heap allocations in the paint loop where possible
- Use `update(QRect)` for partial repaints when only the cursor changes
- Scrollback operations should be O(1) amortized (deque, not vector)
- Glyph atlas eviction: clear and rebuild when full (simple, avoids fragmentation)
- Session serialization uses qCompress (zlib level 6) for space efficiency
- Status-bar pollers that read filesystem metadata (git HEAD, foreground process)
  must cache per-cwd for 5+ seconds -- poll timer fires every 2s and synchronous
  reads on network mounts can stutter the UI

## Security

- Limit buffer sizes for external input (OSC strings, inline images, combining chars per cell)
- Validate file paths before opening (resolve relative paths, check existence)
- Use `QProcess::startDetached()` with argument lists, never shell command strings
- Config file written with 0600 permissions
- OSC 52 clipboard query (read) disabled by default -- only write is allowed
- URIs from OSC 8 hyperlinks sanitized before opening (restrict to http/https/file/mailto schemes)
- CSI parameters capped at 32 entries; individual values capped at 16384
- SSH bookmarks do not store passwords -- user authenticates interactively
- Lua plugins sandboxed: no filesystem/network/process access from scripts
- AI API keys stored in config (0600); never logged or displayed

## Testing

- Manual testing for UI interactions across all themes
- Each new widget should be independently instantiable
- Theme changes verified visually across all themes
- Build must compile cleanly with no warnings (`-Wall -Wextra`)
- CTest must pass (`cd build && ctest --output-on-failure`). New audit rules
  land with a fixture dir under `tests/audit_fixtures/<rule-id>/` containing
  `bad.*` (`// @expect <rule-id>` markers on each expected match) and
  `good.*` (no matches). Register the rule pattern in `tests/audit_self_test.sh`.

## Audit Dialog Standards

- Check catalogue entries use the three helpers: `addGrepCheck` / `addFindCheck`
  / `addToolCheck`. Ad-hoc `m_checks.append({...})` only when no helper fits
  (multi-command pipelines with language-specific shapes)
- All security-category checks inherit the `kCommonNoiseExcludes` filter —
  the shared helper applies it; hand-rolled entries must replicate
- Qt-specific semantic checks belong to clazy — do NOT re-add regex checks for
  findChild misuse, connect-capture lifetime, old-style-connect, or container-
  inside-loop. Those produce false positives clazy avoids
- Every check contributes a `source` string via `sourceForCheck()` so the
  multi-tool correlation step can detect cross-tool hits
- `kSourceScannedChecks` lists checks whose output we run through the comment/
  string-aware filter. External linters (cppcheck, clang-tidy, clazy, pylint …)
  are comment-aware natively — never add them to this set
- SARIF export is the canonical machine-readable format; HTML is presentation-
  only and derived from the same per-finding data. SARIF emits
  `physicalLocation.contextRegion` for ±3 snippet lines plus a
  `properties.blame` bag — external tools (GitHub Code Scanning, SonarQube)
  consume these via the sarif-tools de-facto convention
- `audit_rules.json` is a trust boundary — its `command` field runs through
  `/bin/bash` unconditionally. Treat it like `.git/hooks`: your repo, your
  commands, no sandbox
- Pipeline order for post-parse filtering must be preserved:
  `suppressed dedup-key` → `applyPathRules` (generated + path rules) →
  `inlineSuppressed` → `recent-only scope` → dedup within check. Changing
  the order breaks invariants (e.g. severity-shifted findings must survive
  generated-file skip *before* inline-suppression checks read the file)
- Inline suppression scan is per-file-cached via `m_fileLineCache` — scans
  are O(1) amortized across findings in the same file. Never bypass the
  cache; a runaway check on a large file can otherwise read it hundreds of
  times
- Generated-file detection (`isGeneratedFile`) is path-based and absolute —
  matches are skipped regardless of any path-rule override. If a user
  genuinely wants findings in `moc_*` they must rename the file
- Confidence score is computed at render time (not stored per Finding on
  disk). Baseline/trend persistence uses dedup keys, not confidence —
  confidence is a display signal, not an identity
- Git blame enrichment times out at 2s per (file, line) and caches in
  `m_blameCache`. Never call `git blame` from the paint path — prefetch
  during `renderResults()` so the UI stays responsive
- AI triage uses the project's existing `Config::aiEndpoint/aiApiKey/aiModel`
  — never add a separate "audit AI" config section. Response must be JSON
  `{verdict, confidence, reasoning}`; any provider wrap (e.g. ```json …```)
  is stripped before parsing

## Git

- Commit messages: imperative mood, concise subject line
- One logical change per commit
- Branch naming: `feature/<name>`, `fix/<name>`, `refactor/<name>`
