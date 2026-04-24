# Feature: manual tab rename survives app restart

## Problem

Right-click on a tab → "Rename Tab…" pins a manual label in
`m_tabTitlePins` (a `QHash<QWidget *, QString>` on `MainWindow`).
Two consumers gate on that pin:

- `titleChanged` handler — shell-driven OSC 0/2 titles are ignored
  while the pin is set.
- `updateTabTitles` 2 s tick — format-driven labels (`{cwd}`,
  `{title}`, `{process}`) are skipped while the pin is set.

That covers the *running* session: Claude Code's per-prompt OSC 0/2
title writes no longer wipe a manual rename (fixed 0.7.17).

**Pre-fix bug (this spec):** the pin map was memory-only. On
`MainWindow::~MainWindow` the `QHash` went away with the window;
`SessionManager::saveSession` serialized scrollback + cwd but never
captured pins. Next launch restored the tab, called `startShell`,
and the freshly-spawned shell's first OSC 0/2 write relabelled the
tab to `~/src/foo` or `bash` or whatever — the manual rename
evaporated across restart.

## External anchors

- [Qt `QDataStream` versioning](https://doc.qt.io/qt-6/qdatastream.html#versioning) —
  V3 format extends V2 by appending a `QString` field. V2 readers
  stop reading at end-of-stream; the new field is invisible to
  them. Same pattern already used for the V1→V2 `cwd` addition.
- [Keep a Changelog — "Removed" vs "Changed"](https://keepachangelog.com/en/1.1.0/) —
  a schema bump that's strictly additive counts as "Changed", not
  "Removed". No downstream data is destroyed; old session files
  keep loading with `pinnedTitle` defaulting to empty.

## Contract

### Invariant 1 — `VERSION = 3` in sessionmanager.h

The schema version constant must be bumped to 3 so older Ants
binaries refuse-to-load V3 files (via the `version > VERSION`
guard in `restore`). Cross-version load-into-older-binary is not
supported; the format is explicitly a personal-preference cache,
not inter-version interchange.

### Invariant 2 — round-trip preserves pinnedTitle

`SessionManager::serialize(grid, cwd, "My Deploy Tab")` →
`SessionManager::restore(grid, bytes, &cwdOut, &pinnedOut)` must
yield `pinnedOut == "My Deploy Tab"`. Empty pin round-trips as
empty (not as a missing-field signal, not as null).

### Invariant 3 — V2 file loads with empty pinnedTitle out-param

If a session file was written by a V2 Ants binary (no `pinnedTitle`
field in the stream), `restore` must succeed and leave the
`pinnedTitle` out-param untouched / empty. `in.atEnd()` gates the
read so the `QDataStream::ReadPastEnd` status bit doesn't flip and
fail the restore.

### Invariant 4 — MainWindow save threads `m_tabTitlePins.value(w)`

`src/mainwindow.cpp` saveAllSessions loop must pass the pin
lookup (`m_tabTitlePins.value(w)`) as the 4th arg of
`SessionManager::saveSession(...)`. A refactor that drops the arg
silently re-introduces the bug.

### Invariant 5 — MainWindow restore applies the pin to `m_tabTitlePins`

`src/mainwindow.cpp` `restoreSessions` must call
`SessionManager::loadSession(..., &savedPinnedTitle)` and assign
`m_tabTitlePins[terminal] = savedPinnedTitle` when the saved pin
is non-empty. Without populating the pin map, the next `titleChanged`
fire would still relabel the tab despite setTabText showing the
right string initially.

## How this test anchors to reality

The test is split in two:

1. **Runtime round-trip:** builds an in-memory `TerminalGrid`,
   calls `serialize(grid, "/home/test", "Deploy")`, then
   `restore(…, &cwdOut, &pinnedOut)` and asserts both out-params.
   Repeats with empty-pin and with a 64-char long pin to check
   no silent truncation in the session layer (truncation is the
   UI layer's job — the 30-char ellipsis happens at
   `setTabText`, not during save/load).
2. **V2 back-compat:** constructs a V2-style stream by hand (magic,
   VERSION=2, minimal grid fields, title, cwd, nothing else),
   asserts `restore` returns true and leaves `pinnedTitle`
   out-param empty.
3. **Source-grep:** verifies sessionmanager.h has `VERSION = 3`,
   mainwindow.cpp's saveAllSessions loop passes pins, and
   restoreSessions assigns the pin to `m_tabTitlePins`.

## Regression history

- **Introduced:** original session-persistence implementation
  (pre-0.6 era). Manual tab rename was added later (0.6.29-ish)
  without also extending the session serializer.
- **Flagged:** 2026-04-24 (user asked "Will the tab renames persist
  across Ants Terminal sessions?").
- **Fixed:** 0.7.19 — V3 schema field `pinnedTitle` appended after
  cwd; MainWindow save/restore threaded.
