# Pseudo-modal dialog blocking (ANTS-1051)

User request 2026-04-28: "when a dialog box is open, only the
dialog box is interactive, anything behind the dialog box should
not be interactive."

In 0.7.50 (ANTS-1083 / commit `6bea531`) the project deliberately
made every dialog **non-modal** to work around QTBUG-79126 — under
KDE Plasma + KWin + Qt 6.11 + a frameless + translucent parent on
Wayland, calling `setModal(true)` causes Qt to drop button clicks
on the dialog itself. Going non-modal kept the dialog responsive
but lost the click-blocking-on-the-parent semantics that modal
dialogs normally provide.

The fix is to emulate modality manually — we can't trust the Qt
modal state machine on this stack, but we can intercept mouse and
key events ourselves and drop the ones that would land outside any
visible dialog.

## Fix

Extend the existing `qApp`-level eventFilter (already installed
for ANTS-1050) with a Mouse/Key-event branch. The decision —
"should this event be suppressed?" — lives in a pure-logic helper
`dialogfocus::shouldSuppressEventForDialog(watched, event)` so the
feature test drives it without instantiating MainWindow + PTY.

Helper returns `true` iff:
- `event->type()` is one of `MouseButtonPress`,
  `MouseButtonRelease`, `MouseButtonDblClick`, `Wheel`,
  `KeyPress`, `KeyRelease` (the full mouse-and-keyboard
  interactive set; `KeyRelease` paired with `KeyPress`
  prevents Qt modifier-state desync where Shift-press is
  swallowed but Shift-release leaks through), AND
- at least one `QDialog` is visible among the top-level
  widgets, AND
- `watched` is a `QWidget` that is **neither** the visible
  dialog itself NOR a descendant of it. Formally, for every
  visible `QDialog d`, the helper requires
  `watched != d && !d->isAncestorOf(watched)`. The explicit
  `watched != d` clause matters because Qt's `isAncestorOf`
  is **strict** — a widget is not its own ancestor — so
  clicks on the dialog's own frame would otherwise be
  suppressed.

When the helper returns `true`, `MainWindow::eventFilter` returns
`true` to swallow the event (rather than calling `QMainWindow::
eventFilter(...)` which forwards). This matches Qt's documented
event-filter convention.

## Cross-cutting with ANTS-1050

ANTS-1050 already added a Close-event branch to
`MainWindow::eventFilter`. ANTS-1051 adds a Mouse/Key-event branch
to the same override; the dispatch is by `event->type()`. One
filter installation, one override, two helper functions in
`src/dialogfocus.h`. INV-6 of ANTS-1050 anticipated this — the
present spec tightens that forward-compat clause into a concrete
contract.

## Stacked-dialog handling

Multiple QDialogs may be visible simultaneously (e.g. Settings →
"Restore Defaults?" confirm). Helper iterates ALL visible dialogs
and allows the event through if it's inside ANY of them. Click on
A while B is also visible: allowed (A is itself a visible dialog,
ancestor check passes). Click on MainWindow chrome while A and B
are both visible: blocked.

## What about the window-manager titlebar / drag / close buttons?

Mouse events on KWin-decorated chrome (close X, minimize, drag
edge) are handled by the window manager BEFORE Qt sees them. The
qApp filter never sees those events, so the user can still
close/move/resize MainWindow while a dialog is up. Custom
in-window titlebar widgets (the frameless-window case) are inside
MainWindow's render tree and DO pass through the filter — those
WILL be blocked, matching the user's "only the dialog is
interactive" ask.

## Out of scope

- Visual feedback (greyed-out parent, blur effect). The user's ask
  is interaction-blocking only.
- Bring-dialog-to-front-on-click-outside. A more polished modality
  would raise the dialog when the user clicks on a blocked area;
  this spec just blocks. Future ANTS-NNNN if requested.
- QMessageBox / QFileDialog from inside a plugin Lua VM or AI
  worker. Those are plain QDialogs and benefit from the same
  blocking; no special-case wiring.
- Wayland-specific input-shape / `xdg-popup` constraint coordination.
  The blocking is at the Qt-event-filter layer, independent of the
  compositor's input routing.
- **Drag-and-drop events** (`DragEnter`, `DragMove`, `Drop`) are
  not on the suppress list. Drag-drop into the terminal while a
  dialog is open is unusual; the user's "interaction" framing
  targets clicks/keys/wheel. If a future user reports drag-drop
  leaking through, add it here.
- **`QShortcut`-driven keybindings** (Ctrl+T new tab, Ctrl+W close
  tab, etc.) ARE blocked while a dialog is visible. They land on
  MainWindow as KeyPress events and pass through the suppress
  filter. This matches the user's "only the dialog is interactive"
  intent — a Ctrl+T while the Settings dialog is open should not
  spawn a new tab. Documented here so a future contributor doesn't
  add a "let through if QShortcut" exception thinking it's
  friendlier.

## Invariants

Source-grep + pure-helper drive (no MainWindow instantiation).

- **INV-1** Pure-logic helper exists with the exact signature
  `bool dialogfocus::shouldSuppressEventForDialog(QObject *watched,
  QEvent *event)` declared as an `inline` free function in the
  `dialogfocus` namespace inside `src/dialogfocus.h`. Asserted by
  source-grep. Discrimination of QDialog uses
  `qobject_cast<QDialog *>` (matching ANTS-1050's idiom in the
  same file).

- **INV-2a** Positive case: with one visible `QDialog` and a
  `QWidget` outside its tree, the helper returns `true` for
  **every** suppressed event type. Asserted by behavioural
  drive over the full enumerated set:
  `MouseButtonPress`, `MouseButtonRelease`,
  `MouseButtonDblClick`, `Wheel`, `KeyPress`, `KeyRelease`.
  Drives the helper once per type and asserts `true` each
  time. Defends against an implementation that handles only
  one type and passes a single-type drive — the gap the
  pass-2 cold-eyes review caught when the suppress list was
  expanded from three types to six.

- **INV-2b** Negation: same setup but the `watched` widget is the
  dialog itself OR a child of the dialog → returns `false`.
  Behavioural drive over both forms.

- **INV-2c** Negation: no visible dialog → returns `false`
  regardless of event type / watched widget.

- **INV-2d** Negation: visible dialog exists, but the event type
  is not mouse/key (e.g. `Show`, `Hide`, `Paint`, `FocusIn`,
  `Close`, `Resize`, `Move`, `Timer`) → returns `false`. The
  explicit inclusion of `Close` in the sample makes INV-5's
  cross-INV claim self-evident from grep — Close events bypass
  the suppress branch and reach ANTS-1050's refocus path
  unchanged.

- **INV-2e** Stacked-dialog: two visible QDialogs A and B.
  `watched` is A (or A's child). Helper returns `false` — clicks
  on A are allowed even though B is also visible.

- **INV-2f** Stacked-dialog negation: two visible QDialogs A and B.
  `watched` is a `QWidget` outside both trees. Helper returns
  `true` — click on chrome behind both dialogs is blocked.

- **INV-2g** Defensive: `nullptr` watched OR `nullptr` event →
  `false`. Both branches asserted. (Qt's eventFilter contract
  guarantees non-null `event`, but the null-event branch is
  belt-and-braces — cheap to test, prevents a future caller
  hitting an unguarded path.)

- **INV-2h** Watched IS the visible dialog itself (not a child)
  → returns `false`. This guards the strict-ancestor edge case:
  Qt's `QWidget::isAncestorOf` is strict (a widget is not its
  own ancestor), so an implementation that checks only
  `!d->isAncestorOf(watched)` and forgets the `watched != d`
  clause would suppress clicks on the dialog's own frame.
  Asserted by behavioural drive: `shouldSuppressEventForDialog
  (dialog, mouseEvent)` returns `false`.

- **INV-3** Mouse/key events handled in `MainWindow::eventFilter`
  via the helper. Asserted by source-grep that `eventFilter`'s
  body references both `dialogfocus::shouldSuppressEventForDialog`
  and the convention "return `true` to swallow" (i.e. an explicit
  `return true;` inside the suppress branch).

- **INV-4** Single shared eventFilter override for ANTS-1050 and
  ANTS-1051. Asserted by source-grep that
  `MainWindow::eventFilter` body references both
  `shouldRefocusOnDialogClose` (ANTS-1050) and
  `shouldSuppressEventForDialog` (this spec).

- **INV-5** Cross-INV: the two helpers
  (`shouldRefocusOnDialogClose` from ANTS-1050 +
  `shouldSuppressEventForDialog` from this spec) are mutually
  exclusive on event type — `Close` vs
  `MouseButton*/Wheel/Key*`. Dispatch order inside
  `MainWindow::eventFilter` is therefore immaterial; both
  helpers are called and at most one returns `true`. INV-2d's
  explicit inclusion of `Close` in its sample guarantees this.

## How to verify pre-fix code fails

```bash
git checkout <impl-sha>~1 -- src/mainwindow.cpp src/mainwindow.h \
                              src/dialogfocus.h
cmake --build build --target test_dialog_pseudo_modal
ctest --test-dir build -R dialog_pseudo_modal
# Expect: every INV fails — pre-fix dialogfocus.h has only the
# ANTS-1050 helper, no shouldSuppressEventForDialog; the
# eventFilter has no mouse/key suppress branch.
```

## Test environment

`tests/features/dialog_pseudo_modal/test_dialog_pseudo_modal.cpp`
runs under `-platform offscreen` (same as
`dialog_close_focus_return`) since the behavioural INVs need
`QDialog::show()` + `isVisible()` + `isAncestorOf()` to behave on
real QWidgets without a display server.
