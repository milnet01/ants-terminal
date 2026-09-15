# Feature: broadcast input stays in one tab and says when it is on

## Invariants

**INV-1 — broadcast reaches the panes of the source's tab only.** The
broadcast callback set in `MainWindow::connectTerminal` finds the source's tab
page with `tabPageOf(m_tabWidget, source)` and writes to that page's
`TerminalWidget` children. It never iterates `liveTerminals()` or
`m_allTerminals`, which hold every terminal in every tab.

**INV-2 — broadcast starts off on every launch.** No MainWindow source calls
`broadcastMode()` or names the `broadcast_mode` config key, and
`m_broadcastMode` is initialised to `false` in `mainwindow.h`. The toggle
changes the running window only.

**INV-3 — a status-bar chip shows while broadcast is on.** `m_broadcastChip` is
added to the status bar. `refreshBroadcastChip()` sets its visibility from
`m_broadcastMode`, and both the Broadcast toggle and `applyTheme()` call it, so
the chip follows the toggle and a theme switch.

**INV-4 — each receiving pane encodes the key for its own modes.** The callback
receives the key event and writes `t->encodeKey(event)` to each target, never
the source's bytes. In `TerminalWidget`, every `sendKeyData` call in
`keyPressEvent` passes the event, and `sendKeyData` hands it to
`m_broadcastCallback(this, event)`. `encodeKey` routes through
`encodeEarlyKey`, which applies `bracketedPaste()` and `encodeKittyKey`, and
`encodeLegacyKey`, which applies `applicationCursorKeys()`.

## Rationale

The callback wrote to every terminal in every tab, while the menu item read
"All Panes". The mode was saved to config and restored at launch, and its only
sign was a menu tick and a status message that cleared itself. Answers typed
into one Claude session reached every other session and every shell.

The bytes forwarded were encoded for the source terminal. Claude Code enables
the Kitty keyboard protocol, so plain shells received codes such as `5;2u` and
ran each line as a garbled command.

## Test surface

`test_broadcast_input.cpp` reads the MainWindow sources
(`ants_test::slurpMainWindow`), `src/mainwindow.h` (`SRC_MAINWINDOW_H_PATH`) and
`src/terminalwidget.cpp` (`SRC_TERMINALWIDGET_PATH`), and checks function bodies
as text with comments stripped. No `MainWindow` or `TerminalWidget` is built:
both need a live PTY.

## Regression history

- **ANTS-5223:** broadcast reached every tab, survived a restart and showed no
  lasting sign. Locked by INV-1 to INV-3.
- **ANTS-5224:** broadcast forwarded bytes encoded for the source terminal.
  Locked by INV-4.
