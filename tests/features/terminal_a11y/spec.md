# Feature: Terminal screen-reader accessibility (ANTS-1078)

Conformance test for the H9 core slice — `TerminalWidget` exposed to
assistive technology via a `QAccessibleInterface` + `QAccessibleTextInterface`
adapter. Full design + invariants: `docs/specs/ANTS-1078.md`.

## Contract

A `TerminalWidget` makes its **visible viewport** readable to a screen
reader: plain-text content, a caret offset, per-character rects, and a
throttled change event — without retaining any per-widget content cache,
and at zero cost when no AT is active.

## Invariants covered (from docs/specs/ANTS-1078.md)

- **INV-1** — with the factory installed + AT active,
  `QAccessible::queryAccessibleInterface(widget)` returns a non-null
  interface with `role() == QAccessible::Terminal` and a non-null
  `textInterface()`.
- **INV-2** — `accessibleText()` is the visible viewport: one line per
  row, trailing spaces trimmed, trailing all-blank rows dropped, rows
  joined by `'\n'`; an all-blank viewport yields `""`.
- **INV-3** — over the text interface, `text(s,e)` equals
  `accessibleText().mid(s, e-s)` in range; `text(0,-1)` returns the whole
  string; `characterCount()` equals `accessibleText().size()`.
- **INV-4** — `accessibleCaretOffset()` / `cursorPosition()` is the
  caret's offset within the viewport text (via
  `effectiveCursorRow()/Col()`), clamped to `[0, characterCount()]`; `0`
  on a blank screen.
- **INV-5** — `textAtOffset(WordBoundary)` returns the word containing the
  offset; both boundary types keep `*startOffset`/`*endOffset` in
  `[0, characterCount()]`.
- **INV-6** — for single-column ASCII cells,
  `accessibleOffsetAt(accessibleRectForOffset(o).center()) == o`;
  `accessibleRectForOffset` of a `'\n'`/out-of-range offset is null;
  `accessibleOffsetAt` returns `-1` for padding / past-trim / dropped
  blank-row points.
- **INV-7** — `notifyAccessibilityChanged()` guards on
  `QAccessible::isActive()` before any text build / event (source check,
  bounded to the function body).
- **INV-8** — the adapter declares no string/buffer content member
  (source check on `terminalaccessible.h`).
- **INV-9** — when active, `notifyAccessibilityChanged()` emits exactly
  one `QAccessibleTextCursorEvent` per call, carrying
  `accessibleCaretOffset()`.

## Scope

In: the read path + caret + throttled event. Out (ANTS-3363): rich text
attributes, OSC-133-D-gated text-inserted events, selection write-back,
wide-cell exact rects. Build: `test_vt` bundle; label `features;fast`.
