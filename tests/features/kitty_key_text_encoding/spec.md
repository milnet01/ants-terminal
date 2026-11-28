# Kitty keyboard protocol: text keys with modifiers

ANTS-6654. With the Kitty keyboard protocol on (`CSI > flags u`),
`TerminalWidget::encodeKey` sends a text key held with Ctrl, Alt or Super as
`CSI code ; modifier u`. The code is the key's own lowercase codepoint, taken
from the key, not from its text: with Ctrl held, Qt's text is the control
byte (Ctrl+a gives `0x01`). Shift alone is not ambiguous, so under flag 1
Shift+a is sent as the text `A`.

The test builds a real widget, turns the protocol on through the parser, and
passes `QKeyEvent`s to `encodeKey`.

## Invariants

- **INV-1 — Ctrl+letter under flag 1.** Ctrl+a sends `CSI 97;5u`, Ctrl+Alt+a
  `CSI 97;7u`.
- **INV-2 — Ctrl+letter under flag 8.** Ctrl+a sends `CSI 97;5u`; a plain
  `a` sends `CSI 97u`.
- **INV-3 — Shift alone under flag 1 is text.** Shift+a sends `A`; Alt+a
  still sends `CSI 97;3u`.

## Regression history

- **ANTS-6654:** the code came from `text()`, so Ctrl+a fell through to the
  legacy `^A` under flag 1 and was sent as `CSI 1;5u` under flag 8; Shift+a
  under flag 1 was sent as `CSI 97;2u`.
