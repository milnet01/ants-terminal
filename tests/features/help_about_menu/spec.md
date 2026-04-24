# Feature: Help → About menu surfaces running version

## Problem

User ask 2026-04-24: "How do I see what version of Ants Terminal I
am running? Can you please add a GUI version (an item in the menubar
called About perhaps)?"

Pre-fix, the only way to read the running version was
`ants-terminal --version` on the command line. The binary sets
`QCoreApplication::applicationVersion(ANTS_VERSION)` at startup
(`main.cpp:164`), and Qt's `QCommandLineParser::addVersionOption`
wires that to `--version` output. Neither the menubar nor any
dialog surfaced this to the user.

`ANTS_VERSION` lives in `CMakeLists.txt`'s
`project(... VERSION X.Y.Z)` as the project-wide single source
of truth — the same symbol already threads through the packaging
files, the man page, and the debug-log session header. Exposing
it through a menubar About entry closes the user-visible gap
without adding a second version string.

## External anchors

- [Qt 6 docs — QMessageBox::about](https://doc.qt.io/qt-6/qmessagebox.html#about)
  — the standard idiom for GUI "About" dialogs.
- [Qt 6 docs — QMessageBox::aboutQt](https://doc.qt.io/qt-6/qmessagebox.html#aboutQt)
  — stock "About Qt" dialog paired with every app's own About.
- [freedesktop.org Menu Bar recommendations](https://developer.gnome.org/hig/patterns/containers/menu-bars.html)
  place the Help menu last on the menubar; About is the last
  standard entry in Help.
- `CLAUDE.md` project convention: *"`project(... VERSION X.Y.Z)`
  in `CMakeLists.txt` is the single source of truth.
  `ANTS_VERSION` macro propagates everywhere — never hardcode
  version strings in `.cpp`/`.h`."* The About dialog must read
  `ANTS_VERSION`, not a local literal.

## Contract

### Invariant 1 — Help menu exists and is last on the menubar

`MainWindow` adds a top-level `&Help` menu to `m_menuBar`. It is
constructed **after** all other menus (File, Edit, View, Split,
Tools, Settings) so Qt places it as the rightmost entry, matching
the Linux desktop HIG and every terminal emulator users are
already familiar with.

### Invariant 2 — Help menu contains an About action

The Help menu contains an `&About Ants Terminal...` action. The
ellipsis is the Qt convention for "opens a dialog"; the ampersand
on `A` sets the keyboard mnemonic so the menu is keyboard-
accessible (`Alt+H`, `A`).

### Invariant 3 — About dialog body references ANTS_VERSION

The About action's handler constructs a `QMessageBox` whose body
includes the running `ANTS_VERSION` value. Asserted via
source-grep: the handler body must reference `ANTS_VERSION` (not
a hardcoded `0.7.21` literal that would drift on every bump).

### Invariant 4 — About dialog is rich-text with clickable links

The handler sets `textFormat(Qt::RichText)` and
`textInteractionFlags(Qt::TextBrowserInteraction)` so the GitHub
URL in the body is clickable. Users shouldn't need to copy-paste
a text URL from an About dialog in 2026.

### Invariant 5 — About Qt action is present and uses Qt's stock dialog

A second Help menu action `About &Qt...` exists and routes to
`QMessageBox::aboutQt`. Inherits future Qt-version bumps
automatically.

### Invariant 6 — ANTS_VERSION is the project-wide single source

Source-grep confirms the About handler does not contain a
hardcoded `0.7.` prefix or a `"Version: 0.7"` literal. The only
version string inside the handler is `ANTS_VERSION`.

## How this test anchors to reality

MainWindow is too heavy to instantiate under a feature test
(wires PTY, tab bar, splitters, many dialogs). This test is
source-grep on `src/mainwindow.cpp`:

1. `m_menuBar->addMenu("&Help")` appears after every other
   `addMenu` call (so Help is the last menu).
2. The Help menu's construction block contains
   `addAction("&About Ants Terminal...")` and
   `addAction("About &Qt...")`.
3. The About handler's body contains `ANTS_VERSION` and the
   GitHub URL.
4. The About handler sets `Qt::RichText` + `Qt::TextBrowserInteraction`.
5. No hardcoded `"0.7."` prefix appears inside the About handler
   body.

If a future refactor replaces the Help menu with a custom
implementation that drops the About action, or switches to a
hardcoded version literal, this test fires.

## Regression history

- **Introduced:** absent from day one — Ants Terminal shipped
  without a GUI-visible version indicator.
- **Flagged:** 2026-04-24 user ask.
- **Fixed:** 0.7.21 — Help menu added with About Ants Terminal
  and About Qt actions; About dialog reads `ANTS_VERSION`
  directly, shows Qt runtime version via `qVersion()`, shows
  Lua engine version when compiled with `ANTS_LUA_PLUGINS`.
