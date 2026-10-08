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

## Rationale

The ANTS-5110 performance pass found each of these. For INV-1 and INV-2:
the size cap applied only when the log opened, so a long session grew the
file without limit, and a FIFO path blocked the worker on open.

## Test surface

`test_diagnostics_guards.cpp` reads `src/debuglog.cpp` and
`src/remotecontrol_workspace.cpp` (located from the test's own path) for
INV-1 and INV-2. INV-3 to INV-6 call `BuildCache::parseBuildOutput` and
`DebugLog` directly, with the debug log in a temporary directory.

## Regression history

- **ANTS-5110:** the defects above. Locked by this spec.
