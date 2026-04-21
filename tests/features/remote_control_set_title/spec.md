# Remote-control `set-title` — pin a tab label

## Contract

Fifth rc_protocol command. Sets the active or specified tab's label
to a chosen string and **pins** it: the value survives both the
per-shell `titleChanged` signal (OSC 0/2 from the inferior) and the
2 s `updateTabTitles` refresh tick. Empty `title` clears the pin and
the auto-title path takes over again.

Request shape:

    {"cmd":"set-title","tab":<int optional>,"title":"<string>"}

- `tab` — optional 0-based index. Omitted → active tab. Validated
  via `QJsonValue::isDouble()` so `--remote-tab 0` stays meaningful
  (consistent with `send-text`).
- `title` — required JSON string. Empty string clears the pin.

Response shape on success:

    {"ok":true,"index":<int>}

`index` echoes the affected tab. Errors:

- `set-title: missing or non-string "title" field`
- `set-title: no tab at index <i>`

The pin persists for the lifetime of the tab — closing the tab
clears the pin entry alongside `m_tabSessionIds` so the
`QHash<QWidget*, QString>` doesn't accumulate dangling pointers.

When the pin is cleared (empty `title`):

- If `tabTitleFormat != "title"`, `updateTabTitles()` immediately
  rebuilds the label from cwd / process per the active format.
- If `tabTitleFormat == "title"` (the default), `updateTabTitles`
  bails, so we restore the most recent shell-provided title from
  `TerminalWidget::shellTitle()` directly. Without that restore
  the pinned label would sit stale until the next OSC 0/2 fires
  (which on a quiet prompt may be never).

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"set-title"` to
  `cmdSetTitle`.
- **INV-2** `cmdSetTitle` rejects a missing or non-string `title`
  via `isString()`.
- **INV-3** `cmdSetTitle` uses `isDouble()` to disambiguate "tab
  omitted" from "tab 0" (consistent with `send-text`); the
  no-tab fallback resolves via `currentTabIndexForRemote()`.
- **INV-4** `MainWindow::setTabTitleForRemote(int, const QString&)`
  is declared public, returns `bool`, non-const.
- **INV-5** The titleChanged-signal lambda in `connectTerminal`
  checks `m_tabTitlePins.contains(...)` before calling
  `setTabText` — without it, the next OSC 0/2 from the shell
  wipes the pin.
- **INV-6** `updateTabTitles` checks `m_tabTitlePins.contains(...)`
  before relabeling — without it, the 2 s tick wipes the pin.
- **INV-7** Tab destruction path (the `m_tabSessionIds.remove(w)`
  site) also calls `m_tabTitlePins.remove(w)` so the QHash
  doesn't accumulate dangling QWidget* keys.
- **INV-8** Empty-title clear path restores from `shellTitle()`
  when `tabTitleFormat == "title"` — without it, the cleared
  pin leaves the stale custom value sitting on the tab strip
  until the shell next sends OSC 0/2.
