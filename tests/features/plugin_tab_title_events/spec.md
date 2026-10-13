# Feature: plugins hear title changes, new tabs and closed tabs

## Invariants

**INV-1 — `title_changed` fires.** The `titleChanged` handler in
`MainWindow::connectTerminal` fires `PluginEvent::TitleChanged` with the new
title, for every pane, whether or not its tab's label is pinned.

**INV-2 — `tab_created` fires for every new tab and pane.**
`MainWindow::newTab`, `MainWindow::newTabForRemote` and
`MainWindow::splitCurrentPane` call `firePluginTabEvent(true, …)`.
`MainWindow::restoreSessions` calls it once per restored tab, after the
default tab is removed, so the index is the tab's final one.

**INV-3 — `tab_closed` fires for every closed tab and pane.**
`MainWindow::performTabClose`, `MainWindow::closeFocusedPane` and the
`shellExited` handler's split-pane branch in `MainWindow::connectTerminal`
call `firePluginTabEvent(false, …)`.

**INV-4 — the payload.** `firePluginTabEvent` fires `PluginEvent::TabCreated`
or `PluginEvent::TabClosed` with `index=N&split=0|1`: the tab's 0-based index,
and 1 when a split pane opened or closed rather than a whole tab.

## Rationale

`ants.on()` accepted all three names and `PLUGINS.md` documented them, but
nothing fired them, so a handler never ran. All three are fire-and-forget
(ANTS-1750 § 2.6). The title rate is bounded: `TerminalWidget` emits
`titleChanged` at most once per VT batch, and only on a change.

## Test surface

`test_plugin_tab_title_events.cpp` reads the `MainWindow` sources
(`slurpMainWindow`) and checks the function bodies as text. No `MainWindow`
is constructed.

## Regression history

- **ANTS-4272:** the three events had no `fireEvent` call site. Locked by
  this spec.
