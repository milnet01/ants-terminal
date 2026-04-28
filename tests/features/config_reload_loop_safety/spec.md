# Feature: config-reload loop safety

## Problem

`MainWindow::onConfigFileChanged` is the slot wired to a
`QFileSystemWatcher` watching `config.json`. The slot reloads the config
and re-applies settings — including `applyTheme(m_config.theme())`,
which until 0.7.50 unconditionally called `Config::setTheme(name)` →
`save()` → wrote the watched file back to disk.

That synchronous save inside the slot enqueues a kernel inotify event.
Qt only reads inotify events from the event loop, which means the event
is dispatched **after** `onConfigFileChanged` returns — outliving the
`m_configWatcher->blockSignals(true/false)` window introduced in 0.7.31
to "prevent the loop". The next `fileChanged` then re-enters the slot,
which re-runs `applyTheme` → `setTheme` → `save`, etc., in an unbounded
loop.

User-observable symptoms:

- Status bar permanently sticks at `"Config reloaded from disk"` —
  every reload restarts the 3 s timer before it can expire.
- `Help → Check for Updates` (and every other transient
  `showStatusMessage` toast) is invisibly stomped within milliseconds.
- `Settings → Preferences` doesn't open: `onConfigFileChanged` calls
  `m_settingsDialog->deleteLater()` to invalidate the cached widget,
  and that runs faster than the user can click `Preferences` again.

The bug recurred after the 0.7.31 attempted fix because `blockSignals`
on a `QFileSystemWatcher` only suppresses *currently emitted* signals;
queued inotify events delivered later sail through unaffected. The
prior fix never actually covered the failure mode — it covered a
hypothetical synchronous re-emission that doesn't happen in practice.

## External anchors

- [Qt docs — `QFileSystemWatcher`](https://doc.qt.io/qt-6/qfilesystemwatcher.html)
  — semantics of `fileChanged`. Uses inotify on Linux; emission is
  driven by Qt's event loop reading the inotify fd, not synchronous
  callbacks from the kernel.
- [Qt docs — `QObject::blockSignals`](https://doc.qt.io/qt-6/qobject.html#blockSignals)
  — blocks emissions while in effect; does **not** queue suppressed
  signals for later delivery, but conversely does not stop the
  underlying event source from continuing to enqueue work that the
  event loop will process *after* the block is released.
- Linux `inotify(7)` — kernel-side queue is opaque to userspace; reads
  are mediated by `read(2)` on the inotify fd, which Qt does inside its
  socket-notifier path each event-loop iteration.

## Contract

### Invariant 1 — `Config::setTheme` is idempotent

When called with a value that already equals `m_data.value("theme")`,
`Config::setTheme` MUST early-return without calling `save()`. This is
the primary loop-breaker: a no-op `applyTheme(currentTheme)` from
inside `onConfigFileChanged` produces no disk write and therefore no
fresh inotify event.

The corresponding source pattern (locked by the test):
`src/config.cpp` `Config::setTheme` body must contain a guard of the
shape `if (m_data.value("theme").toString() == name) return;` before
the unconditional `m_data["theme"] = name; save();` lines (or
equivalently, the body must short-circuit through the
`storeIfChanged` helper).

### Invariant 1b — every `Config` setter is idempotent

The 0.7.51 fix made *only* `setTheme` idempotent because that's the
one that broke. But every `Config` setter that ends in `save()`
shares the same shape, and any of them could trigger the same
inotify-loop class of bug if a future `onConfigFileChanged` ever
re-applies them with the existing value. The 0.7.51 follow-up
(post-debt-sweep) generalises idempotence to **every public setter
that calls `save()`**: `setFontSize`, `setOpacity`, `setSessionLogging`,
`setEditorCommand`, `setEnabledPlugins`, `setHighlightRules`,
`setTabGroups`, `setKeybinding`, `setPluginSetting`,
`setWindowGeometry`, `setRawData`, and the ~40 others.

The recommended source pattern is the private helper
`Config::storeIfChanged(const QString &key, const QJsonValue &value)`
which encapsulates compare-then-assign:

```cpp
void Config::setX(T value) {
    if (!storeIfChanged("x_key", value)) return;
    save();
}
```

For compound sub-object setters (`setKeybinding`, `setPluginGrants`,
`setPluginSetting`, `trustAuditRulePack`) an inline guard reading the
specific sub-object field is correct.

For multi-field setters (`setWindowGeometry`'s 4 ints) the body must
call `storeIfChanged` per field with bitwise-OR aggregation so every
field is written, then `save()` only when at least one differed.

Locked functionally: the test sweeps one setter per shape (bool, int
with qBound, double, QString, QStringList, QJsonArray, QJsonObject,
compound sub-object, compound-nested, multi-field) and asserts that a
double-set with the same value does not change the file's mtime.

### Invariant 1c — `storeIfChanged` returns false on match

`Config::storeIfChanged(key, value)` returns false (and leaves
`m_data` untouched) when the existing value at `key` already equals
`value`; returns true (and assigns) otherwise. Tested via observable
side-effect (mtime), since the helper is private.

### Invariant 2 — `onConfigFileChanged` skips no-op `applyTheme`

`MainWindow::onConfigFileChanged` MUST compare the freshly-loaded
theme against `m_currentTheme` and skip the `applyTheme(...)` call
when the value is unchanged. Defense-in-depth and a small efficiency
win on trivial reloads (e.g. window-geometry tick re-saving the same
file).

The expected source pattern: a `if (newTheme != m_currentTheme)
applyTheme(newTheme);` (or equivalent) line inside
`onConfigFileChanged`.

### Invariant 3 — `onConfigFileChanged` carries a re-entrancy guard

`MainWindow` MUST own a `bool m_inConfigReload` member and
`onConfigFileChanged` MUST early-return when it is already `true`.
The guard is cleared on the next event-loop tick via
`QTimer::singleShot(0, this, [this]() { m_inConfigReload = false; })`
so any inotify event queued by a save *inside* the slot is dropped,
while a subsequent genuine external edit is accepted normally.

This is belt-and-braces protection: invariants 1 + 2 prevent the
loop in the known path, but the guard catches any future setter that
forgets to be idempotent.

### Invariant 4 — the failed `blockSignals` attempt does not return

`onConfigFileChanged` MUST NOT call
`m_configWatcher->blockSignals(true)` /
`m_configWatcher->blockSignals(false)`. The 0.7.31 fix was ineffective
(see Problem); leaving it in place obscures the real fix and invites a
future contributor to re-introduce the false sense of safety.

The test asserts the slot body contains neither
`m_configWatcher->blockSignals(true)` nor
`m_configWatcher->blockSignals(false)`.

## Regression history

- **0.7.31** — `blockSignals(true/false)` bracketing introduced as the
  attempted fix. Reported by user as still broken in 0.7.50.
- **0.7.50** (this fix, 2026-04-28) — root cause traced to
  `setTheme(...) → save()` re-entering inotify after `blockSignals`
  releases. Idempotent setter + re-entrancy guard + no-op skip in the
  reload path. Locked down by this test.

## Test strategy

Source-grep based, mirroring `github_status_bar` and
`help_about_menu`. The slot's behavior under inotify is hard to
exercise from a unit test (would need a real filesystem watcher, real
inotify, and a long-running event loop with timing assertions);
asserting the call shape catches the regression at the call-site
that produces it. The `Config::setTheme` idempotence is also
exercised functionally — construct a Config, call `setTheme` twice
with the same value, observe that the file's mtime does not change on
the second call.
