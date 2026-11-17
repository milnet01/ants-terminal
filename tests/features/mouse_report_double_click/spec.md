# A double-click goes to the app while it has mouse reporting on

ANTS-6532. Qt delivers the second press of a double-click as a
double-click event, not a press. While the app in the terminal has mouse
reporting on (DECSET 1000, 1002 or 1003), that press belongs to the app, as
the first one does: `mouseDoubleClickEvent` reports it with
`sendMouseEvent` and selects nothing. Shift overrides reporting, as it does
for a single press.

The test builds a real widget, prints a word, and sends it a double-click.

## Invariants

- **INV-1 — reporting off, a double-click selects the word.** The control
  case: `selectedText()` is the word.
- **INV-2 — reporting on, a double-click selects nothing.** After DECSET
  1000, the same double-click leaves `selectedText()` empty.
- **INV-3 — Shift overrides reporting.** With reporting on, Shift+double-click
  selects the word.

## Not covered

The report bytes `sendMouseEvent` writes need a live PTY, which this test
has none of.
