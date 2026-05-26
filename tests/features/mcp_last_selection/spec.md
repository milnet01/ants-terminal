# Feature test — `last_selection` MCP terminal-selection read (ANTS-1312)

Contract for the `last_selection` MCP tool — returns the focused (or
explicitly-routed) terminal's current selection so Claude can pull the
error / stack trace / snippet the user highlighted without walking the
scrollback to re-find it.

## What this test locks

**Wiring contract** (source-grep — the live behaviour is delegated to
`TerminalWidget::selectedText()` which already has its own coverage):

- **INV-1** — `remotecontrol.h` declares `cmdLastSelection`;
  `remotecontrol.cpp` defines `RemoteControl::cmdLastSelection`.
- **INV-2** — `mainwindow.cpp` registers `last_selection` via
  `registerToolProvider` with `CallerCwdContract::TabSpecific`.
- **INV-3** — `claudeintegration.cpp` carries the tool descriptor under
  the `"last_selection"` name, with a `caller_cwd` property in the input
  schema (mirroring `recent_errors`).
- **INV-4** — `claudeintegration.cpp` lists `last_selection` in
  `tabSpecificAcceptsTabIndex` so an explicit `tab:N` overrides the
  caller_cwd anchor.
- **INV-5** — `claudeintegration.cpp` carries a `typical_token_cost`
  entry for `last_selection` (selections are small; the entry signals
  to callers that this is a cheap read).
- **INV-6** — `claudeintegration.cpp`'s `kindForName` classifies
  `last_selection` in the `"terminal"` bucket (alongside `get_text` /
  `recent_errors`).
- **INV-7** — `claudeintegration.cpp`'s `callerCwdContractFor` returns
  `C::TabSpecific` for `last_selection`.
- **INV-8** — `cmdLastSelection` calls `TerminalWidget::selectedText()`
  (the existing selection accessor) — sole source of truth, no
  reimplementation.

**Response envelope shape** (verified by inspection of the
`cmdLastSelection` body — no live runner needed):

- **INV-9** — On `has_selection == false` the envelope sets `text` to
  the empty string (not omitted) so callers can branch uniformly on
  `has_selection`.
- **INV-10** — When no terminal resolves, refuses with
  `code:"no_window"` (same shape as `recent_errors`).

Exit 0 = every invariant holds.
