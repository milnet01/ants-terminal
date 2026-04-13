# Project Rules

## Core Principles

1. **User-first design** -- Every feature should make the terminal more pleasant and productive to use
2. **Extensibility** -- New features should plug in without rewriting existing code
3. **Consistency** -- All UI elements respect the active theme
4. **Reliability** -- The app should never crash from user actions (paste, input, resize, rapid output)
5. **Performance** -- Rendering must stay smooth even with high-throughput terminal output
6. **Security** -- No data leaves the system without explicit user action

## Development Rules

1. All colors must come from the active theme -- no hardcoded hex values in widget code
2. All persistent state goes through the `Config` class
3. All inter-widget communication uses Qt signals/slots
4. New widgets must work with all existing themes
5. The terminal always retains focus after any action (menu, dialog, tab switch)
6. Window state (size, position, theme, font size) persists between sessions
7. Use `std::unique_ptr` or Qt parent-child ownership for all heap allocations -- no raw `new`/`delete`
8. Validate and bound all external input (PTY data, clipboard, config values)

## Architecture Rules

1. `VtParser` emits `VtAction` structs -- it never modifies grid state directly
2. `TerminalGrid` processes `VtAction`s -- it never touches rendering or widgets
3. `TerminalWidget` renders the grid -- it never modifies grid state except through the parser
4. `MainWindow` wires components -- it owns tabs, menus, and config application
5. Split pane hierarchy: QTabWidget > QSplitter (nested) > TerminalWidget
6. `GlRenderer` handles GPU rendering -- it reads grid state but never modifies it
7. `LuaEngine` is sandboxed -- plugins communicate only through the `ants` API
8. `AiDialog` is self-contained -- it makes network calls only when the user clicks Send
9. `SessionManager` serializes read-only snapshots -- it never modifies grid state during save

## Feature Development Process

1. Discuss the feature scope before implementation
2. New VT sequences go in `VtParser` + `TerminalGrid`
3. New UI features go in `TerminalWidget` or `MainWindow`
4. New dialogs get their own header/source pair
5. Wire into `MainWindow` via signals/slots
6. Test with all themes
7. Update CLAUDE.md if architecture or patterns change

## Audit Rules

1. Project-specific policy checks (e.g. "all openUrl calls must front-load a
   scheme allowlist") live in `audit_rules.json` at the project root, not in
   `auditdialog.cpp`. Hardcoded checks are reserved for generic patterns worth
   shipping to every user of the app
2. Regex rule IDs and patterns must stay in sync with fixtures under
   `tests/audit_fixtures/<rule-id>/`. When a pattern changes in
   `auditdialog.cpp` it must also change in `tests/audit_self_test.sh`
3. Never re-add a regex check that clazy covers semantically (findChild misuse,
   connect-3arg-lambda, container-inside-loop, old-style-connect, qt-keywords)
4. `.audit_suppress` is append-only during normal operation — an entry, once
   written, documents a conscious decision. Cleanup is manual
5. Baselines (`.audit_cache/baseline.json`) represent "accepted state at time T".
   Regenerate only when the state materially changes, not to mask new findings

## Code Quality

1. No unused imports or dead code
2. No `// TODO` without an associated issue or task
3. Keep methods under 40 lines where practical
4. Comments only where logic is non-obvious -- no docstring boilerplate
5. Prefer `std::clamp`, `std::min`, `std::max` over manual bounds checks
6. Use `static_cast` over C-style casts

## Theme Rules

1. Every theme must define all fields present in the `Dark` theme struct
2. Theme names are title-cased display names
3. Accent colors should have sufficient contrast against their theme's background
4. ANSI palette colors (0-15) must be defined for every theme

## Security Rules

1. OSC string buffer capped at 10MB to prevent memory exhaustion
2. Inline image count capped to prevent memory exhaustion
3. File paths from terminal output must be validated before opening
4. Config file permissions set to 0600 (owner read/write only)
5. No shell command string construction -- use argument lists with QProcess
6. OSC 52 clipboard read (query `?`) disabled -- only write is allowed
7. OSC 8 URIs must be scheme-validated before opening (http, https, file, mailto only)
8. Combining characters per cell capped at 8 to prevent DoS
9. CSI parameters capped at 32 entries, values capped at 16384
10. UTF-8 overlong encodings, surrogates, and out-of-range codepoints rejected (replaced with U+FFFD)
11. SSH bookmarks never store passwords -- authentication is interactive
12. AI API keys stored in 0600-permission config, never logged
13. Lua plugins sandboxed: `os`, `io`, `debug`, `require`, `loadfile`, `setmetatable`, `coroutine` removed from environment
14. Lua execution timeout: 10 million instruction limit per event handler
15. Network requests only made with explicit user action (AI Send button)
16. Network timeout: 30 seconds on all outgoing HTTP requests
17. Session files validated on load: all sizes bounds-checked, max 100MB compressed
18. GL resources cleaned up explicitly with context current -- never in destructors
19. CSI 20t/21t (title reporting) and DECRQSS (DCS $q) intentionally NOT implemented -- prevents escape injection (CVE-2024-56803)
20. Bracketed paste sanitizes embedded `\e[201~` from pasted text to prevent paste injection (CVE-2021-28848)
21. Config and session files written atomically via temp file + rename
22. SIGPIPE ignored globally -- PTY write errors handled via return codes
23. OSC 52 clipboard write capped at 1MB decoded to prevent abuse-style pastes
24. AI endpoint scheme validated -- plaintext `http://` warns once per session
    when an API key is configured (localhost is permitted silently for Ollama/LM Studio)
25. UTF-8 accumulator writes are atomic -- `appendUtf8()` computes encoded
    length first and aborts cleanly instead of partial writes that overshoot maxBytes

## Plugin Rules

1. Plugins live in `~/.config/ants-terminal/plugins/<name>/`
2. Each plugin must have an `init.lua` entry point
3. Optional `manifest.json` provides metadata (name, version, description, author)
4. Plugins can only interact via the `ants` API -- no direct access to Qt or terminal internals
5. Event handlers returning `false` cancel the event (keypress interception)
6. Plugin errors are logged, never crash the terminal

## Release Checklist

1. All themes render correctly
2. Image paste auto-saves and inserts filepath
3. Split panes create, focus, and close correctly
4. Tab management works (new, close, switch, reorder)
5. Window position/size/theme persists across restarts
6. Config file is valid JSON after all operations
7. Build compiles with zero warnings
8. Scrollback, search, and selection work across all modes
9. Mouse reporting works with TUI apps (htop, lazygit, fzf)
10. DA/CPR responses work (test with `echo -e '\e[c'` and `echo -e '\e[6n'`)
11. Combining characters render correctly (test with accented text)
12. Undercurl renders in Neovim with LSP diagnostics
13. Focus reporting works (test with vim's `set termguicolors`)
14. Configurable keybindings load from config.json
15. GPU rendering toggle works without artifacts
16. Ligatures render correctly with JetBrains Mono / Fira Code
17. SSH manager connects to remote hosts
18. AI assistant sends/receives with configured endpoint
19. Lua plugins load and fire events
20. Session persistence saves and restores scrollback
21. Background alpha/transparency works with compositor
22. Sixel and Kitty graphics render correctly
