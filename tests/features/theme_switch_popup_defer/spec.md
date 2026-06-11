# Feature: theme switch defers the app-wide restyle out of a popup's event loop

**ANTS-2097.** Selecting a theme from the View→Themes `QMenu` crashed
the app with a SIGSEGV inside `QApplication::setStyleSheet`.

## Problem

`QAction::triggered` for a theme item fires *synchronously* inside the
`QMenu`'s mouse-event / nested-event-loop context. `MainWindow::applyTheme`
then calls `qApp->setStyleSheet(...)`, which walks Qt's global widget set
and re-polishes every widget. As the menu tears down it reaps a
`deleteLater()`'d transient status-bar widget (the ANTS-1893 toast / Undo
button, or a Claude permission prompt) mid-walk — invalidating Qt's
iterator, so the polish loop dereferences a freed widget pointer.

Coredump signature: SIGSEGV at `testb $1,0x30(%rax)` with `rax = 0x31`
(a garbage pointer), frame #1 `QApplication::setStyleSheet`, frame #2
`MainWindow::applyTheme`. Recurred across coredumps 2026-06-05, -06-06,
-06-11. The ANTS-2024 `sendPostedEvents(DeferredDelete)` reap was
insufficient — it is the menu's *own* nested loop, not a pending
`DeferredDelete`, that performs the teardown.

## Fix

At the top of `applyTheme`, after the same-theme early-return: when
`QApplication::activePopupWidget()` is non-null, re-post the call via
`QTimer::singleShot(0, this, …)` and return. The deferred turn runs after
the menu has closed and the event stack has unwound, so the restyle walks
a quiescent widget set. Startup / programmatic callers (no active popup)
stay synchronous.

## Invariants tested

- **INV-1** — `applyTheme` references `QApplication::activePopupWidget()`,
  and that reference appears BEFORE the `qApp->setStyleSheet(` call in the
  function body (the guard gates the restyle, not the reverse).
- **INV-2** — the guard defers via `QTimer::singleShot(0` and `return`s,
  so the synchronous path is skipped while a popup is open.
- **INV-3** — the same-theme early-return (`name == m_currentTheme`)
  still precedes the popup guard, so a no-op theme re-select neither
  defers nor restyles.

## Method

Source-grep on `src/mainwindow.cpp` (`SRC_MAINWINDOW_CPP_PATH`): locate
the `MainWindow::applyTheme` body and assert ordering of the early-return,
the `activePopupWidget()` guard, the `singleShot(0` deferral, and the
`qApp->setStyleSheet(` call. A behavioural GUI repro (open the Themes
menu, click a theme with a live transient status-bar widget) is the real
acceptance check and is verified by hand on relaunch — this test locks the
guard against silent removal.
