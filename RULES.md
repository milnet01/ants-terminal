# Project Rules

## Core Principles

1. **User-first design** -- Every feature should make the terminal more pleasant and productive to use
2. **Extensibility** -- New features should plug in without rewriting existing code
3. **Consistency** -- All UI elements respect the active theme
4. **Reliability** -- The app should never crash from user actions (paste, input, resize, rapid output)
5. **Performance** -- Rendering must stay smooth even with high-throughput terminal output

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

## Feature Development Process

1. Discuss the feature scope before implementation
2. New VT sequences go in `VtParser` + `TerminalGrid`
3. New UI features go in `TerminalWidget` or `MainWindow`
4. Wire into `MainWindow` via signals/slots
5. Test with all themes
6. Update CLAUDE.md if architecture or patterns change

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
