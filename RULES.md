# Project Rules

## Core Principles

1. **User-first design** — Every feature should make the terminal more pleasant to use
2. **Extensibility** — New features should plug in without rewriting existing code
3. **Consistency** — All UI elements respect the active theme
4. **Reliability** — The app should never crash from user actions (paste, input, resize)

## Development Rules

1. All colors must come from the active theme dict — no hardcoded hex values in widget code
2. All persistent state goes through the `Config` class
3. All inter-widget communication uses Qt signals/slots
4. New widgets must work with all existing themes
5. The input area always retains focus after any action
6. Window state (size, position, theme) persists between sessions

## Feature Development Process

1. Discuss the feature scope before implementation
2. Add new modules in `app/` for self-contained features
3. Wire into `MainWindow` via signals/slots
4. Test with all themes
5. Update documentation (CLAUDE.md, STANDARDS.md) if patterns change

## Code Quality

1. No unused imports or dead code
2. No `# TODO` without an associated issue/task
3. Keep methods under 30 lines where practical
4. Docstrings on classes and non-obvious public methods

## Theme Rules

1. Every theme must define all keys present in the `Dark` theme
2. Theme names are title-cased display names
3. Accent colors should have sufficient contrast against their theme's background
4. Test new themes against: welcome message, user messages, images, scrollbars

## Release Checklist

1. All themes render correctly
2. Image paste works from clipboard
3. Shift+Enter creates new line; Enter submits
4. Window position/size persists across restarts
5. Config file is valid JSON after all operations
6. No Python warnings or deprecation notices on startup
