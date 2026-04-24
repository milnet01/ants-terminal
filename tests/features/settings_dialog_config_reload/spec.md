# Feature: Settings dialog discarded on external config reload

## Problem

`MainWindow` caches the `SettingsDialog` across Preferences... opens so
subsequent opens are instant. The dialog is constructed with a pointer
to `MainWindow::m_config` and populates every tab's widgets from the
then-current `m_config` values at construction time.

When the user edits `~/.config/ants-terminal/config.json` out of band
(text editor, `jq` pipeline, sync tool), the `QFileSystemWatcher` at
`m_configWatcher` fires and `onConfigFileChanged` runs:

```cpp
m_config = Config();   // whole struct reassigned from disk
```

The `&m_config` pointer handed to the cached dialog is *not* dangling
— `m_config` is a value-member and keeps a stable address. But the
dialog's widgets still hold the pre-reload values. A subsequent
Preferences... open shows a dialog whose font spinbox, theme combo,
keybindings table, and trigger/highlight grids all show what the
config *was* before the external edit. If the user then clicks OK,
`settingsChanged` fires and `applyTheme`/`applyFontSize` replay those
stale values over the just-reloaded fresh ones — the external edit
is silently undone.

This is the "cached dialog + dangling `&m_config`" item from the
0.7.12 /indie-review hardening sweep. The fix is the simplest and
safest: destroy the cached dialog on config reload so the next open
rebuilds it from the fresh `m_config`.

## External anchors

- [CWE-672 — Operation on a Resource after Expiration or Release](https://cwe.mitre.org/data/definitions/672.html):
  close analogue — the dialog operates on widget state that has
  logically expired (its Config view became stale).
- [Qt docs — QFileSystemWatcher](https://doc.qt.io/qt-6/qfilesystemwatcher.html):
  documents the external-edit reload mechanism but makes no
  guarantees about observers of the reloaded state. Callers must
  invalidate their own caches.
- 0.7.12 /indie-review Tier 2 finding: *"Cached dialog + dangling
  `&m_config`. `mainwindow.cpp:1479` constructs
  `SettingsDialog(&m_config, this)` once; `onConfigFileChanged`
  reassigns `m_config = Config()`. Reopening Settings after an
  external edit of `config.json` dereferences a dangling reference.
  Fix: destroy cached dialog on config reload."*

## Contract

### Invariant 1 — `onConfigFileChanged` invalidates the cached dialog

After `onConfigFileChanged` reloads `m_config`, the cached
`m_settingsDialog` pointer is nulled (or the dialog is destroyed)
so the next `settingsAction` open rebuilds it from the fresh
`m_config`.

### Invariant 2 — open dialogs are closed on reload

If the Settings dialog is currently *visible* when
`onConfigFileChanged` fires, it is closed (not silently swapped
underneath the user's cursor). The user sees the transition and
knows to reopen if they want to compare their widget state to the
just-reloaded values.

### Invariant 3 — destruction routes through deleteLater, not raw delete

The dialog is scheduled for deletion via `deleteLater()`, not
`delete`. A raw delete would double-free if the dialog's own
`finished` signal handler (or any still-in-flight slot invocation)
re-enters. `deleteLater()` defers destruction until the next event-
loop tick, after any already-queued slots drain.

### Invariant 4 — cache-invalidation is in `onConfigFileChanged`,
not `onConfigChanged`

The external-edit reload path is the only place where `m_config` is
wholesale-reassigned; internal setter mutations (`m_config.setX()`)
leave the widget state consistent with the Config object because
the dialog's setters are what wrote those values. The invalidation
must live in the external-edit path only, or we'd needlessly
destroy the dialog on every in-app config change.

## How this test anchors to reality

AuditDialog and SettingsDialog are GUI-heavy Qt widgets; constructing
a full MainWindow in a feature test is prohibitive. We source-grep
`src/mainwindow.cpp` for the invariants:

1. Inside `onConfigFileChanged`, the tokens
   `m_settingsDialog->deleteLater()` and `m_settingsDialog = nullptr`
   both appear, between the function opener and its closer.
2. The cached dialog's visibility is checked via `isVisible()` and
   closed if true (I2).
3. `deleteLater` is used rather than `delete m_settingsDialog`
   (I3 — negative grep for `delete m_settingsDialog`).
4. The invalidation is scoped to `onConfigFileChanged` only —
   confirmed by checking that no other function in `mainwindow.cpp`
   contains the same `m_settingsDialog = nullptr` assignment
   *except* where the dialog's own `finished` signal legitimately
   clears it (0.7.12 code pattern; not the case here but guarded in
   case it lands later).

If the cache-invalidation is removed, the invariants 1–3 fail.
If someone moves the invalidation to the wrong function, invariant
4 fails.

## Regression history

- **Introduced:** 0.6.x when `SettingsDialog` caching landed
  (removing the per-open re-construction to avoid Settings-dialog
  boot latency).
- **Flagged:** 2026-04-23 /indie-review Tier 2 hardening sweep.
- **Fixed:** 0.7.20 — `onConfigFileChanged` closes (if visible),
  `deleteLater()`s, and nulls `m_settingsDialog` before re-applying
  settings, so the next Preferences... open rebuilds the dialog
  from the freshly reloaded `m_config`.
