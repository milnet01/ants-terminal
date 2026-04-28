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

The handler builds a `QDialog` containing a `QLabel` whose
`textFormat` is `Qt::RichText`, whose `textInteractionFlags`
include `Qt::LinksAccessibleByMouse`, and which has
`setOpenExternalLinks(true)` so the GitHub URL in the body
opens in the user's browser when clicked.

The dialog uses a plain `QPushButton` labelled "OK" inside an
`QHBoxLayout` row, NOT a `QDialogButtonBox`. The button's
`clicked()` signal is connected to `QDialog::close`. The
`QDialogButtonBox` path was retired in 0.7.50 because under
KDE/KWin + Qt 6.11 + our frameless + `WA_TranslucentBackground`
MainWindow on Wayland, the role-based dispatch path
(`QDialogButtonBox::accepted` / button(Role)→clicked) was
observed to silently drop clicks alongside the modal-grab
issue (per QTBUG-79126). A plain `QPushButton::clicked` →
`close` connection is what the bg-tasks / settings dialogs
already use successfully on this stack.

`Qt::TextSelectableByMouse` is intentionally NOT set on the
label — there's nothing in the About body the user needs to
select, and including it (which is what `Qt::TextBrowserInteraction`
implies) was part of the click-stealing surface that regressed
the dialog in 2026-04-25.

### Invariant 5 — About Qt action is present and uses the same dialog pattern

A second Help menu action `About &Qt...` exists. As of 0.7.49 it
**does not** use `QMessageBox::aboutQt` — that helper internally
calls `QMessageBox::exec()`, which on KDE/KWin + Qt 6.11 + our
frameless + `WA_TranslucentBackground` MainWindow does not surface
the dialog as the active input window, so the OK button silently
no-ops (user report 2026-04-27). The action constructs a custom
`QDialog` mirroring the About Ants Terminal pattern (heap +
`WA_DeleteOnClose` + `show()` + `raise()` + `activateWindow()`)
and renders the Qt runtime + build versions plus a link to
`qt.io/licensing`.

### Invariant 6 — ANTS_VERSION is the project-wide single source

Source-grep confirms the About handler does not contain a
hardcoded `0.7.` prefix or a `"Version: 0.7"` literal. The only
version string inside the handler is `ANTS_VERSION`.

### Invariant 7 — Both About dialogs use the heap+show, non-modal pattern

Both the About Ants Terminal and About Qt handlers construct
their dialog as `auto *dlg = new QDialog(this)` with
`Qt::WA_DeleteOnClose`, leave the dialog non-modal (no
`setModal(true)` call), and call `dlg->show()` followed by
`dlg->raise()` and `dlg->activateWindow()`. Neither uses
`QDialog dlg(this)` + `dlg.exec()`.

The blocking-`exec()` pattern was retired in 0.7.49: the nested
`QEventLoop` `exec()` opens does not, on KDE/KWin + Qt 6.11
under a frameless + `WA_TranslucentBackground` parent, surface
the dialog as the active input window. The `setModal(true)`
attempt that 0.7.49 introduced was retired in 0.7.50 — Wayland
has no equivalent of `Qt::ApplicationModal` (QTBUG-79126,
QTBUG-90005), and the modal grab actually re-introduced the
click-drop. The non-modal `show()` pattern delegates dialog
activation to KWin in the normal way and the OK click reaches
its connected slot.

The OK button is a plain `QPushButton` (NOT
`QDialogButtonBox::Ok`) whose `clicked()` signal connects
directly to `QDialog::close`. This bypasses the role-dispatch
surface that QTBUG-79126 calls out as an aggravating factor
on Wayland.

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
4. The About handler sets `Qt::RichText` on a `QLabel`, sets
   `Qt::LinksAccessibleByMouse` on its `textInteractionFlags`,
   calls `setOpenExternalLinks(true)`, constructs a plain
   `QPushButton` (not `QDialogButtonBox`) and wires its
   `clicked()` to `QDialog::close`.
5. No hardcoded `"0.7."` prefix appears inside the About handler
   body.

If a future refactor replaces the Help menu with a custom
implementation that drops the About action, or switches to a
hardcoded version literal, this test fires.

## Regression history

- **Introduced:** absent from day one — Ants Terminal shipped
  without a GUI-visible version indicator.
- **Flagged:** 2026-04-24 user ask.
- **Fixed:** 0.7.22 — Help menu added with About Ants Terminal
  and About Qt actions; About dialog reads `ANTS_VERSION`
  directly, shows Qt runtime version via `qVersion()`, shows
  Lua engine version when compiled with `ANTS_LUA_PLUGINS`.
- **Re-fixed:** 0.7.35 — original About used `QMessageBox::Ok`
  with `Qt::TextBrowserInteraction`. User reported 2026-04-25
  that the OK button silently did nothing under our frameless +
  WA_TranslucentBackground MainWindow on KDE/KWin + Qt 6.11
  (only the window-manager X dismissed the dialog). Replaced
  with a custom `QDialog` + `QDialogButtonBox::Ok` whose
  `accepted` signal is explicitly connected to `QDialog::accept`,
  and a `QLabel` whose `textInteractionFlags` are narrowed to
  `LinksAccessibleByMouse | LinksAccessibleByKeyboard` (no
  `TextSelectableByMouse`). `setOpenExternalLinks(true)` was
  also added — the previous QMessageBox path made the link
  visually clickable but never wired the URL handler, so the
  GitHub link click was a no-op too. Locked by the I4
  invariant changes above.
- **Re-fixed again:** 0.7.49 — user reported 2026-04-27 that
  *both* About dialogs again silently swallowed OK clicks. The
  0.7.35 fix had only changed the dialog *type* (QMessageBox →
  QDialog); both dialogs still went through `exec()`, which
  opens a nested `QEventLoop`. On KDE/KWin + Qt 6.11 under our
  frameless + `WA_TranslucentBackground` MainWindow, the nested
  loop does not promote the dialog to active input window, so
  the OK button receives no focus and the click silently no-ops
  (the user has to dismiss via the WM close button, exactly as
  in the 2026-04-25 report). Both About dialogs were converted
  to heap + `WA_DeleteOnClose` + `show()` + `raise()` +
  `activateWindow()`, mirroring the pattern already used by
  the Background Tasks, Settings, and Claude Transcript dialogs
  on this stack — which work correctly under the same
  parent-window flags. The OK button's `clicked()` signal is
  also wired directly to `QDialog::accept` in addition to the
  existing `QDialogButtonBox::accepted` connection. The About
  Qt dialog dropped `QMessageBox::aboutQt` (whose internal
  `exec()` is the same regression shape) for a custom QDialog
  with the same heap+show treatment. Locked by I5b (negative
  grep on `QMessageBox::aboutQt`) and I7 (heap+show pattern).
- **Re-fixed yet again:** 0.7.50 — user reported 2026-04-28 that
  *both* About OK buttons (still) did nothing under 0.7.49.
  Diagnosis from prior fixes was wrong on two counts: (a)
  `setModal(true)` is a no-op on Wayland's xdg-shell — there
  is no concept matching `Qt::ApplicationModal` (per
  [QTBUG-79126](https://bugreports.qt.io/browse/QTBUG-79126)
  and [QTBUG-90005](https://bugreports.qt.io/browse/QTBUG-90005))
  — and the modal-grab side effect on KWin/Wayland actually
  routes the OK click into the modal handler and drops it; (b)
  `QDialogButtonBox`'s role-dispatch path is a documented
  aggravator on the same bug. Replaced both About dialogs with
  the proven-working bg-tasks shape: non-modal QDialog + plain
  `QPushButton` whose `clicked()` connects to `QDialog::close`.
  Same fix applied to the Roadmap dialog (the user's third
  report in the same message). Locked by I4 (plain QPushButton
  shape) and I7f (negative grep on `setModal(true)`).
