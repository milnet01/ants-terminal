# Remote-control `select-window` — switch the active tab

## Contract

Fourth rc_protocol command. Switches the active tab to the given
index. Named after Kitty's equivalent command; "window" in Kitty
parlance is what Ants calls a "tab".

Request shape:

    {"cmd":"select-window","tab":<int required>}

- `tab` — required. 0-based index matching the `index` field returned
  by `ls`. `--remote-tab <i>` client-side.

Response shape on success:

    {"ok":true,"index":<int>}

`index` echoes the now-active tab for client convenience.

Errors (each `{"ok":false,"error":"<msg>"}`, exit code 2):

- `select-window: missing or non-integer "tab" field`
- `select-window: no tab at index <i>`

After the switch, the terminal in the newly-active tab receives
keyboard focus via `QWidget::setFocus()`, so follow-up `send-text`
calls without an explicit `tab` field land on the expected pane.
Without that focus restore, a stale focus owner (menubar, dialog,
search bar) would consume the next keystrokes.

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"select-window"` to
  `cmdSelectWindow`.
- **INV-2** `cmdSelectWindow` rejects a missing or non-integer
  `tab` field with a `missing or non-integer "tab" field` error
  envelope. No implicit default-to-active — the user always spells
  out which tab they want, matching Kitty's required-match semantics.
- **INV-3** `cmdSelectWindow` uses `QJsonValue::isDouble()` to
  validate. A `toInt()`-based check would silently accept a
  string-typed `tab` field as 0 and switch to tab 0 when the
  caller's intent was malformed input.
- **INV-4** `MainWindow::selectTabForRemote(int)` is declared public,
  returns `bool` (false on out-of-range), and is non-const (mutates
  the tab widget + focus state).
- **INV-5** `selectTabForRemote` sets focus on the newly-active
  terminal via `setFocus()` — without it, follow-up `send-text` calls
  without an explicit tab would hit a stale focus owner (menubar,
  search bar, a dialog's default button).
- **INV-6** `selectTabForRemote` bounds-checks against
  `m_tabWidget->count()` and returns false on failure — the
  dispatch layer turns that into the out-of-range error envelope.
