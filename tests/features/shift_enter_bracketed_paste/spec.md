# Shift+Enter bracketed-paste byte-sequence contract

## Contract

When the user presses **Shift+Enter** (without Ctrl) inside a terminal
tab, `TerminalWidget::keyPressEvent` constructs a byte sequence and
writes it to the PTY (and, if set, to the broadcast callback). That
sequence must satisfy *all* of the following invariants:

1. **Bracketed-paste path (DECSET 2004 active).**
   When `m_grid->bracketedPaste()` returns `true`, the emitted bytes
   MUST be exactly the 13-byte sequence:

   ```
   { 0x1B, 0x5B, 0x32, 0x30, 0x30, 0x7E,   // ESC [ 2 0 0 ~
     0x0A,                                  // \n
     0x1B, 0x5B, 0x32, 0x30, 0x31, 0x7E }   // ESC [ 2 0 1 ~
   ```

   i.e. `ESC[200~\nESC[201~`. The total size MUST equal 13.

2. **Non-bracketed-paste fallback.**
   When `m_grid->bracketedPaste()` returns `false`, the emitted bytes
   MUST be exactly the 2-byte sequence `{ 0x16, 0x0A }` — readline's
   quoted-insert prefix followed by LF. Total size MUST equal 2.

3. **Ctrl+Shift+Enter is NOT intercepted here.**
   The Shift+Enter handler MUST gate on `!(mods & ControlModifier)`.
   Ctrl+Shift+Enter is reserved for the scratchpad dialog (handler
   earlier in `keyPressEvent`, see `terminalwidget.cpp` ~line 1345).
   If Shift+Enter ran unconditionally on `ShiftModifier`, it would
   shadow the scratchpad keybinding.

4. **Closing end-paste marker must be present.**
   On the bracketed-paste path, the last 6 bytes of the emitted sequence
   MUST be exactly `ESC [ 2 0 1 ~` — `{ 0x1B, 0x5B, 0x32, 0x30, 0x31, 0x7E }`.
   This is the invariant that the length-truncation bug directly
   violated: a hand-coded 8-byte length dropped the closing
   `[201~`, leaving an orphan `ESC` that kept the shell in bracketed-
   paste mode and ate the next keystroke.

## Rationale

Regression origin (0.6.26): the sequence was written as

```cpp
seq = QByteArray("\x1B[200~\n\x1B[201~", 8);
```

The string literal is 13 bytes, but the explicit length argument said 8.
`QByteArray(const char*, int)` truncates to that length — so the PTY
only ever saw `ESC [ 2 0 0 ~ \n ESC`, dropping the `[201~` closing
marker. The shell (readline in bracketed-paste mode) stayed in paste
state forever. Net observable symptom: "the terminal tab freezes after
Shift+Enter — I cannot type further."

The fix replaces the length-coupled two-argument constructor with
`QByteArrayLiteral(...)`, which derives the size from the literal at
compile time. That closes the entire class of "length argument drifts
away from literal" bugs, not just this instance.

## Scope

**In scope:**
- Byte-level equality for both the bracketed-paste and non-
  bracketed-paste sequences.
- Length assertions (13 and 2 bytes respectively).
- The final 6-byte end-paste marker (directly catches the 0.6.26
  truncation bug).
- Verifying the source-level implementation uses a size-coupled
  literal form (`QByteArrayLiteral` or equivalent) so a future edit
  cannot silently re-introduce the same truncation bug.
- Verifying Ctrl+Shift+Enter does not enter this code path
  (source-level: the `!(mods & ControlModifier)` gate is present).

**Out of scope:**
- How bash / readline / Claude Code's Ink TUI *interpret* the
  sequence. This test locks what we emit, not downstream semantics.
- The surrounding Kitty keyboard protocol encoder. That has its own
  coverage.
- Ctrl+Enter (no Shift) behaviour — a different branch of
  `keyPressEvent` handles that.

## Regression history

- **Introduced:** 0.6.26 — first added the bracketed-paste defence
  with a hand-coded byte-length of 8 instead of the literal's 13.
- **Fixed:** 0.6.30 (working tree at time of test authoring, not yet
  committed) — switched to `QByteArrayLiteral(...)` so the size is
  always coupled to the literal.
- **User report:** "tab freezes after pressing Shift+Enter — cannot
  type further", reproducible in any shell with DECSET 2004 active
  (bash 4.4+, any modern TUI).

## Test strategy

Instantiating a full `TerminalWidget` would pull in QOpenGLWidget,
a live PTY, the VtParser, the GL renderer, and half of MainWindow's
indirect dependencies. The keyPressEvent is also `protected`, which
would require subclassing. Instead, the test:

1. Asserts the exact byte sequences that *any* correct implementation
   must produce, using a local reference function that mirrors the
   contract.
2. Performs a source-level inspection of
   `src/terminalwidget.cpp` to confirm:
   - The Shift+Enter handler (lines ~1433-1450) uses
     `QByteArrayLiteral("\x1B[200~\n\x1B[201~")` — i.e. the size is
     derived from the literal, not hand-coded.
   - The Shift+Enter handler gates on `!(mods & Qt::ControlModifier)`
     so Ctrl+Shift+Enter still reaches the scratchpad handler.
   - No `QByteArray("\x1B[200~...", <digit>)` form remains anywhere in
     the file — the specific anti-pattern that caused the 0.6.26 bug.

The source-level check is the direct regression guard: the 0.6.26 bug
was a `QByteArray(literal, 8)` that had the wrong length. Grepping for
any `QByteArray("\x1B[200~..", <number>)` would have caught it at
commit time.
