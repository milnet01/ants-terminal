# Focus-redirect menu/popup guard

## Context

Ants Terminal's `MainWindow` installs a `QApplication::focusChanged` handler
that auto-returns keyboard focus to the active terminal whenever focus lands
on "chrome" widgets (status bar, tab bar, the main window itself). This was
added in 0.6.26 so that after clicking a status-bar button, the next
keystroke reaches the terminal without an extra click.

The original handler excluded `QDialog`, `QMenu`, `QMenuBar`,
`CommandPalette`, and text-input widgets from the redirect by walking the
new-focus-widget's parent chain. That gating is correct when focus lands
directly on one of those widgets — but Qt's menu stack produces brief,
spurious `focusChanged` cycles while the user simply hovers across menubar
items. During those cycles, `now` may momentarily be the bare main window
or an ancestor chrome widget, which would pass the `shouldRedirect` test
and pull focus back to the terminal. Symptom: the File / Edit / View hover
highlight flashes on every mouse motion tick — as reported 2026-04-19.

## Invariants

These guards must live in `mainwindow.cpp`'s focus-redirect lambda (and its
deferred `QTimer::singleShot` fire path). Removing any of them re-opens
the hover-flicker bug.

### INV-1 — popup guard (queue-time)

The focusChanged lambda body MUST early-return when
`QApplication::activePopupWidget()` is non-null. A QMenu that has popped
up is an active popup; the user is navigating it and focus must not be
redirected mid-navigation.

### INV-2 — menu-hover guard (queue-time)

The focusChanged lambda body MUST early-return when the widget under
`QCursor::pos()` has a `QMenu` or `QMenuBar` ancestor. The menubar itself
is NOT a popup (it's a child widget), so `activePopupWidget()` alone does
not cover the hover-over-menubar case.

### INV-3 — popup guard (fire-time)

The `QTimer::singleShot(0, ...)` body inside the focusChanged lambda MUST
also early-return on `activePopupWidget()`. A popup may have opened
between queue and fire.

### INV-4 — menu-hover guard (fire-time)

The `QTimer::singleShot(0, ...)` body MUST also early-return on the
menu-ancestor walk under the cursor. Same rationale as INV-3 for the
menubar-hover case.

### INV-5 — existing guards preserved

Removing this guard must not regress the existing set:
dialog / QMenu / QMenuBar / CommandPalette / text-input parent-chain
skip (from 0.6.26), button-focus skip (2026-04-18 fix for Review-Changes
click-swallow), mouseButtons-down skip, activeModalWidget skip,
visible-dialog skip at fire time.

### INV-6 — visible-dialog guard (queue-time)

The queue-time body MUST also walk `QApplication::topLevelWidgets()` and
early-return if any visible widget inherits `QDialog`. Reason:
`QMessageBox::exec()` / `QDialog::exec()` sets modality during a brief
handshake and `activeModalWidget()` can return null for a tick during
`show()`. Without this guard, the paste-confirmation dialog's Paste
button swallows mouse clicks — only the `&Paste` keyboard shortcut
works (reported 2026-04-19). The same walk already exists at fire time;
INV-6 mirrors it at queue time.

## Failure-mode hook

If a future change consolidates the focusChanged logic into a helper
function, move this spec-check accordingly — but the invariants stand:
popup-open and mouse-over-menu must both short-circuit the redirect.
