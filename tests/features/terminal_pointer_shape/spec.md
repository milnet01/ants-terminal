# Terminal pointer shape — one pointer over the text

**Status:** locks ANTS-6853. Decision: the mouse pointer over the terminal
text is always the I-beam. The hand shows only while the pointer is over a
detected URL and the program is not capturing the mouse.

## Rationale

`TerminalWidget` set the pointer only at the end of `mouseMoveEvent`, and
only after the mouse-reporting branch had returned. A new widget therefore
showed Qt's default arrow, and a tab whose program turned mouse reporting on
(`ESC[?1000h`, `?1002h`, `?1003h`) kept the arrow for as long as reporting
lasted. The user saw some tabs with an I-beam and others with an arrow.

## Scope

In scope: the shape `TerminalWidget::cursor().shape()` reports after
construction and after a plain mouse move (no buttons, no modifiers).

Out of scope: the shape over the scroll bar, search bar, prompt chips and
other child widgets; the shape while a selection drag is running; the
shape of the window frame; mouse-report encoding.

## Invariants

- **INV-1 — a new widget shows the I-beam before any mouse event.**
- **INV-2 — while the program has mouse reporting on, a plain mouse move
  leaves the pointer an I-beam.** Each of `ESC[?1000h` (clicks),
  `ESC[?1002h` (clicks and drags) and `ESC[?1003h` (all motion).
- **INV-3 — with reporting off, a move over plain text gives the I-beam.**
- **INV-4 — with reporting off, a move over a URL gives the hand; once the
  program turns reporting on, a move over the same spot gives the I-beam
  again.** The hand must not outlive the program's capture of the mouse.

## Test

`test_terminal_pointer_shape.cpp` in the `test_chrome` bundle, suite
`TerminalPointerShape`. It feeds bytes through the real `VtParser` into the
widget's grid, delivers `QMouseEvent` moves with `QApplication::sendEvent`,
and reads `cursor().shape()`. No event loop runs.

Before the fix INV-1 and INV-2 fail (the pointer is the default arrow);
INV-3 passes and guards the fix; INV-4 fails at its second half (the hand
is never reset).

## Regression history

Introduced with the first `mouseMoveEvent` that set a pointer only on its
non-reporting path. Fixed under ANTS-6853.
