<!-- ants-config-hot-reload-standards: 1 -->
# Ants Terminal config hot-reload standard

Project-local contract for the `config.json` file watcher and the
in-app writers that share the file with it. Not part of the shareable
`/start-app` standards set — it depends on `MainWindow`'s
`m_configWatcher` (`QFileSystemWatcher`) → `onConfigFileChanged` slot
and the single `Config::save()` write chokepoint.

## The hazard

`MainWindow` watches `config.json` so that an **external** hand-edit
(or a second Ants instance) hot-reloads live. But the app writes the
same file constantly — every `Config` setter calls `save()`. The
watcher cannot see *who* wrote, only *that* it changed, so without care
it treats the app's own writes as external edits and runs the full
external-reload reaction: reload `m_config`, **tear down the cached
Settings dialog**, re-apply the theme, and show a "Config reloaded"
toast.

That teardown is the bug behind ANTS-1981 — the Settings dialog closed
itself on a tab-switch or Apply, because both persist config and so
tripped the watcher against its own dialog.

## C1 — In-app writes must not trigger the external-reload reaction

A write the app itself performed (a setter → `save()`) MUST NOT cause
`onConfigFileChanged` to reload + tear down open UI + toast. Only a
genuine external edit may.

**Mechanism — self-write detection by content (ANTS-1981).**
`Config::save()` records the exact bytes it wrote (`m_lastWrittenBytes`,
exposed via `lastWrittenBytes()`) — but only *after* the atomic rename
succeeds; a save skipped on a latched load-failure or lock contention
leaves the prior value untouched. `onConfigFileChanged` reads the file
and, when the on-disk bytes equal the app's last write, returns early —
it is our own inotify echo, not an external edit. The check runs **after**
the slot's `m_inConfigReload` re-entrancy guard and the watch re-add,
not before them:

```cpp
{   // scoped so cf/onDisk don't leak into the rest of the slot
    QFile cf(path);
    if (cf.open(QIODevice::ReadOnly)) {
        const QByteArray onDisk = cf.readAll();
        if (!onDisk.isEmpty() && onDisk == m_config.lastWrittenBytes()) {
            QTimer::singleShot(0, this, [this]() { m_inConfigReload = false; });
            return;             // self-write echo — skip reload + teardown
        }
    }
}
```

This works because every in-app write routes through `Config::save()`
on the *same* `Config` object the watcher reads, so after a self-write
`lastWrittenBytes()` already holds the on-disk bytes. A real external
edit produces different bytes and falls through to the reload.

This composes with — and does not replace — the two pre-existing
loop guards: the `m_inConfigReload` re-entrancy flag (deferred-cleared)
and idempotent compare-then-write setters (`storeIfChanged`).

## C2 — New config-writing UI inherits the contract for free

Any new component that mutates config while visible (a dialog, an
inline editor) is automatically protected **as long as it writes
through a `Config` setter / `Config::save()`** — the self-write
detection in C1 covers it with no per-component wiring.

- **Do** write config exclusively through `Config` setters.
- **Do NOT** hand-write `config.json` from a component, bypassing
  `Config::save()` — such a write is not recorded in
  `lastWrittenBytes()` and would be misread as an external edit,
  tearing down the very component that wrote it.
- A component that must react to *external* edits should do so via
  `onConfigFileChanged`'s reload path, not by polling the file.

## Limitations

- **Coalesced events compare against the latest write only.** An Apply
  fires several setters → several `save()` calls; `m_lastWrittenBytes`
  holds the *last* one. inotify may coalesce the burst into one event,
  which the check sees against the final on-disk bytes (= the last
  write) — so it still resolves as a self-write. An event seen
  mid-burst may compare against a not-yet-final file and fall through
  to a (harmless, idempotent) reload.
- **The watcher read is unlocked.** `Config::save()` serialises writers
  with `ConfigWriteLock`, but `onConfigFileChanged` reads the file
  without it. The atomic `rename(2)` makes a torn read unlikely; a
  same-tick external edit racing a self-write may still be misread.

**Falsify C1:** open Settings, click Apply (or switch tabs) — the
dialog MUST stay open. A close is the C1 regression signature.

---

## Checklist for a new config-writing component

1. Write only through `Config` setters (never a raw file write), so the
   self-write detection in C1 recognises it. (C1, C2)
2. If the component is long-lived and cached (like the Settings
   dialog), confirm a tab-switch / Apply / toggle does not close it —
   that is the C1 regression signature.
3. Genuine external-edit handling belongs in `onConfigFileChanged`'s
   fall-through reload, not a second watcher. (C2)
