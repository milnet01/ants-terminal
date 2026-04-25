# Feature: Plugin manifest cap + canonical plugin path

## Problem

Two latent issues in `PluginManager::scanAndLoad`:

### 1. OOM via oversized `manifest.json`

Pre-fix `f.readAll()` loaded the entire file into a `QByteArray`
before `QJsonDocument::fromJson` got a chance to reject it. A
malicious plugin (or a corrupted disk leaving a plugin's
manifest at billions of bytes) would allocate that much RAM and
the process would OOM-kill before the JSON parser ran. Real
manifests are <10 KiB.

### 2. Symlink escape via `m_pluginDir`

Pre-fix the scan used:

- `QDir::entryList(QDir::Dirs | QDir::NoDotAndDotDot)` — without
  `NoSymLinks`, so a symlink under `~/.config/ants-terminal/plugins/`
  pointing at `/etc` would be treated as a candidate plugin
  directory.
- Plain string concatenation `m_pluginDir + "/" + pluginDirName`
  for the per-plugin path — no canonicalization. If the user's
  `m_pluginDir` itself was a symlink, every plugin path inherited
  the unresolved alias.

A user who pasted a hostile plugin tarball that contained a
symlink (`evil -> /etc/cron.daily`) would have Ants attempt to
load `init.lua` from `/etc/cron.daily/init.lua`. Bounded blast
radius (the user already trusted the tarball), but defence in
depth.

## Fix

### Manifest cap

```
constexpr qint64 kMaxManifestBytes = 1024 * 1024;  // 1 MiB
if (f.size() > kMaxManifestBytes) {
    qWarning("...");
} else {
    QJsonDocument doc = QJsonDocument::fromJson(
        f.read(kMaxManifestBytes), &err);
    ...
}
```

1 MiB ≈ 250 plugins worth of permission/description text;
real manifests are tiny so the cap never bites legitimate
content.

### Canonical plugin path

```
QString canonicalRoot = QFileInfo(m_pluginDir).canonicalFilePath();
if (canonicalRoot.isEmpty()) canonicalRoot = m_pluginDir;
const QString canonicalRootPrefix = canonicalRoot + "/";
QDir dir(canonicalRoot);
QStringList dirs = dir.entryList(
    QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
```

For each entry, additionally verify the resolved path stays
inside the canonical root:

```
const QString canonicalPlugin =
    QFileInfo(pluginPath).canonicalFilePath();
if (canonicalPlugin.isEmpty() ||
    !(canonicalPlugin == canonicalRoot ||
      canonicalPlugin.startsWith(canonicalRootPrefix))) {
    qWarning("...resolves outside the plugin root...");
    continue;
}
```

`NoSymLinks` is the cheap first-pass filter (rejects entries
whose own name resolves through a symlink). The canonical-path
check is the catch-all (bind mounts, hardlinks under different
names, future filesystem features).

## Contract

### Invariant 1 — 1 MiB manifest cap

`src/pluginmanager.cpp` `scanAndLoad` body must contain a
`kMaxManifestBytes` constant equal to `1024 * 1024` and gate
the JSON parse on `f.size() > kMaxManifestBytes`. The actual
read MUST use `f.read(kMaxManifestBytes)` (bounded), not
`f.readAll()` (unbounded).

### Invariant 2 — `QDir::NoSymLinks` filter

The `entryList` call passes `QDir::NoSymLinks` alongside
`QDir::Dirs | QDir::NoDotAndDotDot`.

### Invariant 3 — canonical root anchor

`scanAndLoad` calls
`QFileInfo(m_pluginDir).canonicalFilePath()` early and uses
the result as the iteration root. A fall-back to the original
path is allowed when canonicalFilePath returns empty (path
doesn't exist — handled identically by the existing
`dir.exists()` check below).

### Invariant 4 — per-entry canonical containment check

For each entry the loop computes
`QFileInfo(pluginPath).canonicalFilePath()` and rejects the
plugin if it doesn't equal the canonical root or start with
`canonicalRoot + "/"`. The reject path emits a `qWarning` and
`continue`s — silently skipping a hostile entry without
aborting the whole scan.

## How this test anchors to reality

Source-grep on `src/pluginmanager.cpp`. The cap pattern is
recognisable by the `kMaxManifestBytes` constant + the
`f.read(kMaxManifestBytes)` call. The path-safety pattern is
recognisable by `canonicalFilePath()` + the `NoSymLinks`
flag + the `startsWith(canonicalRootPrefix)` containment
check.

## Regression history

- **Latent:** since the plugin loader shipped (0.6.x).
- **Flagged:** ROADMAP "Lua: cap manifest size + canonical
  plugin path" — `pluginmanager.cpp:138-143`.
- **Fixed:** 0.7.33.
