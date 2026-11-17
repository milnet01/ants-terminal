# Legacy key encoding carries Alt, Shift and Ctrl

ANTS-6531. With the Kitty keyboard protocol off, `TerminalWidget::encodeKey`
sends xterm's bytes for each key. Alt prefixes the key's bytes with ESC, as
xterm does with `metaSendsEscape`, so readline's Alt+B, Alt+D and
Alt+Backspace reach the shell. A cursor or function key held with Shift, Alt
or Ctrl carries xterm's modifier parameter, `1 + Shift 1 + Alt 2 + Ctrl 4`:
`CSI 1;m X` for a key whose final is a letter, `CSI n;m ~` for the rest.

The test builds a real widget and passes `QKeyEvent`s to `encodeKey`.

## Invariants

- **INV-1 — Alt prefixes ESC on a text key.** Alt+b sends `ESC b`.
- **INV-2 — Alt prefixes ESC on Backspace.** Alt+Backspace sends `ESC DEL`.
- **INV-3 — Alt prefixes ESC on a Ctrl chord.** Ctrl+Alt+a sends `ESC ^A`.
- **INV-4 — a modified arrow carries the parameter.** Shift+Up sends
  `CSI 1;2 A`, Alt+Left `CSI 1;3 D`, Ctrl+Alt+Left `CSI 1;7 D`. With
  application cursor keys on (DECCKM), Shift+Up still sends `CSI 1;2 A`.
- **INV-5 — a modified function key carries the parameter.** Shift+F1 sends
  `CSI 1;2 P`, Shift+F5 `CSI 15;2 ~`, Ctrl+Delete `CSI 3;5 ~`.
- **INV-6 — an unmodified key is unchanged.** `b` sends `b`, Up `CSI A`, F1
  `SS3 P`, Shift+a sends `A`.

## Not covered

Alt+F, Alt+E, Alt+V, Alt+S and Alt+T open menus, and Alt+1..9 switch tabs.
Qt takes those as shortcuts before the widget sees the key.
