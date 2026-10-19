# ANTS-5774 — Review Changes keeps other code's links to its Copy button

## Problem

When the diff loaded, `diffviewer::show()`'s `finalize` called
`copyGuard->disconnect()` before wiring the Copy Diff button's click
handler. A bare `disconnect()` severs every connection the button has,
not only its own click handlers.

Qt's style-sheet engine holds one of those connections. In Qt 6.11,
`QStyleSheetStyle::initWidget` (`qstylesheetstyle.cpp`) connects a
widget's `destroyed` signal to the style-cache cleanup when it first
styles the widget. It does not reconnect while the widget stays styled.
With the link cut, deleting the button left its address in the style
cache. The next app-wide restyle, `qApp->setStyleSheet()` from a theme
switch or a New Window, read the dead pointer and crashed with SIGSEGV
in `QApplication::setStyleSheet`.

## Invariant

**INV-1.** After the diff renders, a connection that other code made to
the Copy Diff button's `destroyed` signal still fires when the dialog is
deleted.

## Test

`test_review_changes_style_cache.cpp` opens the real dialog on a
throwaway git repo with one changed file. It connects a probe to the
Copy Diff button's `destroyed` signal, waits for the render, closes the
dialog and reaps the deferred delete. The probe must have fired.

The probe stands in for Qt's own cleanup link, because Qt keeps the
cache private.
