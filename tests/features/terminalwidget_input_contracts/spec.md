# Feature: every key reaches the shell the same way

## Invariants

**INV-1 — `keyPressEvent` sends keys only through `sendKeyData`.** The body of
`TerminalWidget::keyPressEvent` contains no direct `ptyWrite(` call and at
least one `sendKeyData(` call.

**INV-2 — `sendKeyData` clears the selection, writes and broadcasts.** Its
body calls `clearSelection()`, `ptyWrite(data)` and
`m_broadcastCallback(this, event)`. The broadcast carries the key event, not
the bytes, so each pane encodes it for its own modes
(`tests/features/broadcast_input`, ANTS-5224).

**INV-3 — a failed shell start leaves no stream behind.** In
`TerminalWidget::startShell`, the `if (!ok)` block sets `m_vtStream` to
`nullptr` before returning, so `hasPty()` is false.

## Rationale

The general key path cleared a stale selection and copied the bytes to the
other panes in broadcast mode. Ctrl+arrows, Ctrl+letters and accepting an
autocomplete suggestion wrote to the PTY and returned early, so broadcast mode
sent Ctrl+C to one pane only. One helper now ends every key path.

`startShell` returned `false` on a failed start but left `m_vtStream` set, so
`hasPty()` stayed true and keys were queued to a stream with no shell.

## Test surface

`test_terminalwidget_input_contracts.cpp` reads `src/terminalwidget.cpp`
(`SRC_TERMINALWIDGET_PATH`) and checks the function bodies as text. No
`TerminalWidget` is constructed: it is a `QOpenGLWidget` with a live PTY, and
`keyPressEvent` is protected.

## Regression history

- **ANTS-5077:** Ctrl key paths and autocomplete skipped the selection clear
  and the broadcast; a failed shell start left `hasPty()` true. Locked by this
  spec.
