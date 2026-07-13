# DirTreeWatcher — raw-inotify directory-tree change watcher (ANTS-3509)

Contract for `src/treewatcher.{h,cpp}`, the live-refresh engine behind the
Review Changes dialog. Replaces a `QFileSystemWatcher` that could not see a
content edit of a child file.

## Why it exists

A headless characterization (2026-07-13) proved Qt's `QFileSystemWatcher`
**directory** watch does not emit on a *content edit* of a file inside the
directory — only on add / remove / rename of entries. So an editor or agent
modifying an existing tracked file fired nothing, and the diff went stale until
a commit or a manual Refresh. Raw inotify on a directory watch **does** report
`IN_MODIFY` for a direct child, so watching directories (not one file per
tracked path) both catches edits and uses ≈4× fewer watches.

## Invariants under test

- **INV-1 (content edit fires)** — with a directory added via `addDirs`, editing
  the contents of an existing file inside it emits `changed()`. This is the exact
  case `QFileSystemWatcher` dropped; it is the regression lock.
- **INV-2 (new file fires)** — creating a new file in a watched directory emits
  `changed()`.
- **INV-3 (debounced + single signal)** — a burst of mutations coalesces into
  at least one `changed()` (the watcher debounces; consumers re-probe once).
- **INV-4 (idempotent addDirs)** — re-adding an already-watched directory does
  not grow `watchCount()`; a non-existent path is skipped, not fatal.
- **INV-5 (`directoriesContaining` purity)** — given `git ls-files -z` output
  (NUL-separated relative paths) and a working-tree root, it returns the sorted,
  de-duplicated set of absolute directories that contain those files. Empty
  input → empty list. This is the gitignore-aware watch set (ls-files already
  excludes ignored trees), so no separate exclude mechanism is needed.
- **INV-6 (consumer wiring)** — `diffviewer.cpp` drives live refresh through
  `DirTreeWatcher` (not `QFileSystemWatcher`), seeds the watch set from
  `git ls-files … --exclude-standard`, and runs its git probes with
  `GIT_OPTIONAL_LOCKS=0` so a read-only `status` can't rewrite `.git/index` and
  trigger a re-probe loop.

## Pass / fail

`test_core` (`ants_add_core_bundle`, `QCoreApplication` event loop) exits 0 iff
every gtest case passes. The behavioral cases pump the event loop with a bounded
timeout so inotify delivery + the 300 ms debounce complete. INV-6 is a
source-scrape in `test_dialogs` against `diffviewer.cpp`.
