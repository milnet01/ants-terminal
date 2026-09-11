# Feature spec: DialogChrome D2–D4 affordances (ANTS-1842)

`docs/standards/dialogs.md` mandates four invariants for every dialog.
D1 (theme chrome) already lived in `DialogChrome::install`; D2 (user-
resizable), D3 (size persists), and D4 (re-center on open) were per-dialog
TODOs that no dialog actually implemented. ANTS-1842 folds all three into
`install` so a single opt-in call satisfies D1–D4 and new dialogs can't
drift.

`install` gains `bool resizable` (default `false`, so every existing call
site is unchanged — D1 only) and a `sizeKey`. When `resizable` is set, a
`ChromeGuard` event filter on the dialog drives D2/D4, and D3 persists the
size through a process-global `Config` registered via
`DialogChrome::setConfig` (mirrors the `setActiveTheme` pattern so free-
function dialogs need no `Config` plumbing).

## Invariants

- **INV-1 / D2 grip.** `install(dlg, …, resizable=true, key)` adds a
  `QSizeGrip` child to the dialog (the frameless window has no OS border
  to drag).
- **INV-2 / D1-only default.** `install(dlg)` / `install(dlg, theme)` with
  `resizable` defaulted adds NO `QSizeGrip` — back-compat for the dialogs
  that haven't opted in.
- **INV-3 / D3 save.** With a `Config` registered via `setConfig`, a close
  of a resizable+keyed dialog persists its current `size()` (width/height
  only) under that key — `Config::dialogSize(key)` returns it afterward.
- **INV-4 / D3 restore.** A resizable+keyed dialog whose key already has a
  saved size is resized to it on first show.
- **INV-5 / Config round-trip.** `Config::setDialogSize` /
  `dialogSize` round-trips a `QSize`; position is never stored.
- **INV-6 / release falls back (ANTS-5036).** With two Configs registered,
  `releaseConfig` on the later one makes D3 use the earlier one. Each
  window registers its own Config, and closing a second window frees it.
- **INV-7 / release the last (ANTS-5036).** After `releaseConfig` on the
  only registered Config, a keyed dialog's first show reads no size.
- **INV-8 / window wiring (ANTS-5036).** `~MainWindow` calls
  `DialogChrome::releaseConfig(&m_config)`.

## Test scope

Behavioral, offscreen Qt. Synthetic `QShowEvent` / `QCloseEvent` drive the
guard. INV-8 is a source scrape of `~MainWindow`, because this bundle cannot
build a `MainWindow` headless. The global `Config` registration is reset to `nullptr` after each
test that sets it so bundle-sibling dialog tests stay isolated.
