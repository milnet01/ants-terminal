# Auto-return focus to terminal on dialog close (ANTS-1050)

User request 2026-04-28: "once any dialog box is closed,
automatically shift focus back to the terminal prompt." Today
every dialog (About, Preferences, Roadmap, Update-confirm, AI,
SSH, Audit, Snippets, Review Changes, …) leaves keyboard focus
on the parent `MainWindow` chrome — the user has to click into
the terminal grid to resume typing.

## Fix

A single application-level event filter on `qApp` watches every
`QEvent::Close` on QDialog descendants. The decision of "should
this Close event trigger a refocus?" lives in a pure-static
helper `dialogfocus::shouldRefocusOnDialogClose(QObject *watched,
QEvent *event)` so it can be tested without spinning up a real
MainWindow + PTY + tab system.

The helper returns `true` iff:
- `event->type() == QEvent::Close`, AND
- `watched` is a `QDialog` instance (cast or `inherits`), AND
- after this dialog closes, no other `QDialog` remains visible
  (so closing dialog B above dialog A doesn't refocus through
  A).

When the helper returns `true`, the filter schedules a focus
restore via a deferred-dispatch primitive (e.g.
`QTimer::singleShot(0, ...)` or `QMetaObject::invokeMethod(...,
Qt::QueuedConnection)` — either is contract-equivalent). The
restore target resolves through `focusedTerminal()` (split-pane-
aware) and is **null-guarded** so an early-startup close (e.g.
config-load failure dialog) doesn't dereference an unconstructed
terminal.

The single-filter pattern means new dialogs added later
automatically benefit — no per-dialog plumbing.

## Cross-cutting with ANTS-1051

ANTS-1051 (pseudo-modal click-blocking) ALSO needs an app-level
event filter on `qApp`. Both items share **one**
`MainWindow::eventFilter(QObject *, QEvent *)` override; the
dispatch branches on `event->type()` (Close → ANTS-1050 path,
MouseButtonPress/Release/KeyPress → ANTS-1051 path). One filter
installation, one override.

## Out of scope

- Restoring focus to a non-terminal widget when the user
  explicitly tabbed into one before opening the dialog. Today
  every keyboard-input destination is the terminal.
- Per-tab focus history beyond the active pane.

## Invariants

Source-grep + pure-helper drive (no MainWindow instantiation
required for the behavioural INVs).

- **INV-1** Application-level event filter installed in
  `MainWindow`'s constructor. Asserted by source-grep that the
  constructor body references `qApp->installEventFilter(this)`
  (or `QApplication::instance()->installEventFilter(this)`).

- **INV-2** Pure-logic helper exists with the exact signature
  `bool dialogfocus::shouldRefocusOnDialogClose(QObject *watched,
  QEvent *event)` declared in `src/dialogfocus.h` as an `inline`
  free function inside the `dialogfocus` namespace. Asserted by
  source-grep on the header. The free-function shape (rather
  than a `MainWindow::` static member) keeps the test target's
  link footprint small — the test includes `dialogfocus.h`
  alone, no transitive Ants headers required.

- **INV-2a** Helper returns `true` for a `QClose` event on a
  `QDialog`-derived object whose closing leaves no other
  `QDialog` visible. Asserted by behavioural drive: construct
  a `QDialog`, post a `QEvent::Close`, call the helper, check
  `true`.

- **INV-2b** Helper returns `false` for a non-`QDialog` widget
  receiving a `QEvent::Close` (e.g. a `QMenu`, a
  `QMainWindow` itself during shutdown). Negation guard that
  prevents `setFocus` calls on a terminal during `MainWindow`
  destruction. Asserted by behavioural drive: post Close on a
  plain `QWidget` and assert `false`.

- **INV-2c** Helper returns `false` when another `QDialog`
  remains visible after the close (stacked-dialog case —
  these dialogs are non-modal per ANTS-1050/0.7.50, so
  "stacked" is the accurate term, not "modal-on-modal").
  Asserted by behavioural drive: construct two QDialogs A
  (visible) and B (also visible), close B, call helper with
  B + Close — must return `false`.

- **INV-2d** Helper returns `false` for non-Close events
  (Show, Hide, MouseButtonPress, etc.). Asserted by behavioural
  drive over a sample of event types.

- **INV-3** Refocus is deferred via a queued-dispatch primitive
  (`QTimer::singleShot(0, ...)` OR `QMetaObject::invokeMethod
  (..., Qt::QueuedConnection)` — either accepted) so the
  dialog finishes its teardown before MainWindow grabs focus.
  Asserted by source-grep that `MainWindow::eventFilter` body
  references one of these forms.

- **INV-4** Restore target is the active tab's focused
  terminal pane. The `focusedTerminal()` helper exists in
  `MainWindow` (or equivalent) and resolves to the
  most-recently-focused pane within the active tab in
  split-pane mode, else the sole terminal of the active tab.
  Asserted by source-grep that the eventFilter body invokes
  `focusedTerminal()` (or equivalent helper) inside the
  deferred-dispatch lambda.

- **INV-5** Null-guard inside the deferred-dispatch lambda.
  The lambda must check that the resolved terminal pointer is
  non-null before calling `setFocus`. Defends against the
  early-startup race where a config-load-failure dialog
  closes before `m_currentTerminal` exists. Asserted by
  source-grep that the lambda body contains both
  `focusedTerminal()` AND a `nullptr` / `if` guard before
  the `setFocus` call.

- **INV-6** Single eventFilter override shared with
  ANTS-1051. Asserted by source-grep that
  `MainWindow::eventFilter`'s body contains a switch/if on
  `event->type()` AND references both `QEvent::Close`
  (this spec) and at least one of
  `QEvent::MouseButtonPress` / `QEvent::KeyPress` (the
  ANTS-1051 path); the test treats this as forward-compat
  with ANTS-1051 — until ANTS-1051 lands, INV-6 just
  asserts the close-only branch exists, and tightens to the
  shared form once ANTS-1051 is committed.

## How to verify pre-fix code fails

```bash
# Replace <impl-sha> with the commit that introduces
# shouldRefocusOnDialogClose + the eventFilter wiring.
git checkout <impl-sha>~1 -- src/mainwindow.cpp src/mainwindow.h
cmake --build build --target test_dialog_close_focus_return
ctest --test-dir build -R dialog_close_focus_return
# Expect: every INV fails — pre-fix MainWindow has no helper,
# no eventFilter override, no qApp filter installation.
```

## Test naming + environment

`tests/features/dialog_close_focus_return/test_dialog_close_focus_return.cpp`
maps to user request 2026-04-28 ("focus returns to the
terminal prompt after any dialog closes"). Anchoring on the
user-visible behaviour, not the implementation idiom — per the
testing standard § 2.1.

The behavioural drives in INV-2a/2c need `QDialog::isVisible()`
to return `true` on QDialogs the test constructs. On
headless / CI runners this requires a `QGuiApplication` (or
`QApplication`) initialised with `-platform offscreen` so
window-server interactions succeed without a display server.
The test wires this in `main()` via
`QApplication app(argc, argv)` plus `setAttribute(
Qt::AA_PluginApplication, false)` — the default offscreen
platform in Qt6 supports `show()` + `isVisible()` correctly.
