# Feature: diagnostics guards — debug log, read_log and build-log parsing

## Invariants

**INV-1 — the debug log rotates when it passes its cap while writing.**
`DebugLog::write` in `src/debuglog.cpp` checks the open file's size after each
write and reopens, which rotates it, once it passes `kMaxLogBytes`.

**INV-2 — read_log refuses a path that is not a regular file.**
`RemoteControl::cmdReadLog` refuses an existing path that is not a regular
file with `bad_path` before reading.

**INV-3 — a build-log note after a blank line is not folded into the error
before it.** `BuildCache::parseBuildOutput` counts it as its own warning
(ANTS-1299's continuation rule).

**INV-4 — the notes of an error past the 50-error cap are dropped.** They are
not appended to the 50th error kept.

**INV-5 — an `ANTS_LOG_ALWAYS` line reaches stderr when no category is on.**
With no category active there is no log file to write it to.

**INV-6 — the debug log reopens its path when the file was rotated away.**
`DebugLog::write` checks that its open file is still the one at the log path,
so it never appends to a `debug.log.1` another instance renamed it to.

**INV-7 — a build cache over `kMaxCacheFileBytes` is not loaded**
(ANTS-6528). `BuildCache::loadBuild` reads one byte past the cap and
returns nothing when it gets it.

**INV-8 — `BuildCache::fromJson` keeps the writer's caps** (ANTS-6528): at
most 50 errors, each message at most 240 characters.

**INV-9 — a `fatal error:` line is an error** (ANTS-6596).
`BuildCache::parseBuildOutput` counts GCC's
`a.cpp:1:10: fatal error: x.h: No such file or directory` as an error with
message `x.h: No such file or directory`, as `recent_errors` does.

**INV-10 — staleness is measured from the build's start** (ANTS-6632).
`BuildCache::staleSinceMs` is `startedAtMs` when the record supplied it, else
`recordedAtMs`, so a file edited while the build ran makes the cache stale.

**INV-11 — a cache with no time is not reported fresh** (ANTS-6632).
`BuildCache::checkStale` with no time returns `staleKnown:false`, and
`BuildCache::addStaleness` writes `stale_unknown:true` for it, never
`stale_walk_capped` (ANTS-6988).

**INV-12 — the message cap never splits a surrogate pair** (ANTS-6632), in
`parseBuildOutput` or in `fromJson`.

**INV-13 — clearing the debug log removes `debug.log.1` too** (ANTS-6632).

**INV-14 — the debug log rotates under `debug.log.lock`** (ANTS-6632). While
another instance holds the lock, this one does not rotate; the size is
checked again once the lock is held.

**INV-15 — a failed rotation turns file logging off** (ANTS-6632). When the
log is still over its cap after a write's rotation, the file closes, stderr
says so once, and no later write reopens it until `clear()`.

## Rationale

The ANTS-5110 performance pass found each of these. For INV-1 and INV-2:
the size cap applied only when the log opened, so a long session grew the
file without limit, and a FIFO path blocked the worker on open.

## Test surface

`test_diagnostics_guards.cpp` reads `src/debuglog.cpp` and
`src/remotecontrol_workspace.cpp` (located from the test's own path) for
INV-1 and INV-2. INV-3 to INV-15 call `BuildCache` and
`DebugLog` directly, with the debug log in a temporary directory.

## Regression history

- **ANTS-5110:** the defects above. Locked by this spec.
- **ANTS-6632:** INV-10 to INV-15, from the 2026-11-06 reviews.
