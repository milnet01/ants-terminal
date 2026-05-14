# ANTS-1326 — Scroll-to-bottom chip resists cascading app-button styles

## Background

The floating "scroll-to-bottom" chip in `TerminalWidget`
(`m_scrollToBottomBtn`) is a 32 × 32 QPushButton positioned at
`(width() - 52, height() - 52)` — a 20-pixel inset from the
widget's right/bottom edges. Its local stylesheet declares only
background, colour, border, border-radius, and font-size.

The application-wide stylesheet (`themedstylesheet::buildAppStylesheet`)
declares for QPushButton:

```
QPushButton {
    background-color: %1; color: %3;
    border: 1px solid %4; padding: 6px 14px;
    border-radius: 4px; min-width: 60px;
}
```

Without explicit overrides on the chip's local rule, the `padding`
and `min-width` cascade in. Qt 6.7+'s stylesheet renderer
**adds the padding to the content box**, so the chip's painted
size becomes:

```
content (32 × 32)
  + padding (14 + 14 horizontal, 6 + 6 vertical)
  + border (1 + 1 each axis)
= 62 × 46 (visual), with geometry still 32 × 32
```

The painted region exceeds the geometry, runs past `width() - 20`,
and clips against the widget's right edge / hides behind the
scrollbar. User-visible symptom: only the left ~half of the chip
renders.

## Contract

**I-1** — `m_scrollToBottomBtn` MUST carry `objectName=
"scrollToBottomBtn"` so its stylesheet can be scoped by ID
without affecting other QPushButtons in the terminal surface.

**I-2** — The chip's stylesheet MUST scope every rule with the
`#scrollToBottomBtn` selector. A bare `QPushButton` selector
would match the app cascade and be subordinate to it (specificity
tied / parent-wins for cascading props).

**I-3** — The chip's stylesheet MUST explicitly declare
`padding: 0` and `min-width: 32px` (the chip's geometry width).
Without these, the app-wide rule's `padding: 6px 14px;` and
`min-width: 60px;` cascade through.

**I-4** — The chip's stylesheet MUST also declare `max-width:
32px; min-height: 32px; max-height: 32px;` so the chip is square
even if a future app rule cascades a different size hint.

**I-5 (regression-lock)** — `terminalwidget.cpp` MUST contain
the literal substrings `QPushButton#scrollToBottomBtn`,
`padding: 0`, and `min-width: 32px` inside the chip stylesheet
initialization block.

## Test

`test_scroll_to_bottom_chip_size.cpp` greps
`src/terminalwidget.cpp` for the four required tokens (I-1
through I-4) plus a negative check that the chip's stylesheet
does NOT use a bare `QPushButton {` selector that could allow
cascade.
