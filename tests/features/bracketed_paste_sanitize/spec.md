# Feature test — a paste cannot end bracketed-paste mode early

With bracketed paste on, `TerminalWidget::performPaste` wraps a paste in
`ESC [200~` … `ESC [201~`, so the shell treats it as text, not as typed
commands. A paste that carries its own end marker would close the bracket
early and run what follows. `stripBracketedPasteMarkers`
(`src/bracketedpaste.h`) removes the markers first.

## Invariants

- **INV-1** — The result contains none of the six markers: `200~` and
  `201~` after the 7-bit `ESC [`, the raw 8-bit CSI byte `0x9B`, or its
  UTF-8 form `C2 9B`.
- **INV-2** — INV-1 holds when removing one marker joins the bytes around
  it into another (ANTS-6442), e.g. `ESC [20` `ESC [201~` `0~`, and across
  the 7-bit and 8-bit forms.
- **INV-3** — A paste with no marker is returned unchanged, including one
  holding other escape sequences.
- **INV-4** — `performPaste` wraps the result of
  `stripBracketedPasteMarkers`, not the raw paste.

INV-1 to INV-3 call the function. INV-4 reads `performPaste`'s source.
