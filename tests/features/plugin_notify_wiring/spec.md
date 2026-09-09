# Feature: `ants.notify()` reaches the host

Test contract for ANTS-4273 (carrying ANTS-1737), deferred from the
cold-eyes pass of 2026-05-21.

## Problem

`ants.notify(title, message)` is documented plugin API. The call reached
`LuaEngine::showNotification`, which `PluginManager::wireEngine` re-emitted as
`PluginManager::showNotification` — and nothing connected that. The signal
stopped at the plugin manager, so the function accepted its arguments, raised
no error and did nothing. `PLUGINS.md` carried a note saying so.

Verified against the source before the fix rather than taken from the bullet:
the emit, the engine-to-manager connection and the absent consumer were each
read in place.

## Invariants under test

- **INV-1** — `MainWindow` connects `PluginManager::showNotification`. Without
  a consumer the whole chain is inert, and that is the defect.
- **INV-2** — the plugin path falls back to the status bar. `PLUGINS.md`
  promises a desktop notification "or falls back to the status bar", so
  `showDesktopNotification` reports whether delivery happened and the plugin
  connection uses the status bar only when it did not. Not *as well as*: two
  notifications for one call is a different defect.
- **INV-3** — one notification path, not two. The OSC 9/777 handler and the
  plugin connection both call `MainWindow::showDesktopNotification`. The
  helper was extracted from the OSC lambda when the plugin became a second
  caller; a copied second implementation is what this pins against.
- **INV-4** — the focus gate stays OUTSIDE the helper. The terminal path
  suppresses a notification while the window is focused; an explicit
  `ants.notify()` from a plugin is an intentional act and `PLUGINS.md`
  documents no gate. Moving the gate into the helper would silently drop
  plugin notifications whenever the window is focused.
- **INV-5** — `PLUGINS.md` no longer describes `ants.notify()` as a no-op.
  The warning was accurate and is now false; a doc that keeps it tells plugin
  authors not to use a working function.

## Test shape

Source scrape, the established pattern for wiring invariants in this repo
(`shell_command_wiring`, `image_paste_uri_list` INV-5). A behavioural test
would need a constructed `MainWindow` — a full window with a live PTY, a tray
and a plugin manager — and the defect is precisely a missing connection, which
is what a scrape reads directly.

Runs in the `test_chrome` bundle, which already defines
`SRC_MAINWINDOW_CPP_PATH`. INV-5 needs `PLUGINS_MD_PATH`, added for it.

Label: `features;fast`.
