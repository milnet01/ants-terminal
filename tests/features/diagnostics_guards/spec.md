# Feature: debug log rotation while running; read_log refuses non-regular files

## Invariants

**INV-1 — the debug log rotates when it passes its cap while writing.**
`DebugLog::write` in `src/debuglog.cpp` checks the open file's size after each
write and reopens, which rotates it, once it passes `kMaxLogBytes`.

**INV-2 — read_log refuses a path that is not a regular file.**
`RemoteControl::cmdReadLog` refuses an existing path that is not a regular
file with `bad_path` before reading.

## Rationale

The ANTS-5110 performance pass found both: the size cap applied only when the
log opened, so a long session grew the file without limit, and a FIFO path
blocked the worker on open.

## Test surface

`test_diagnostics_guards.cpp` reads `src/debuglog.cpp` and
`src/remotecontrol_workspace.cpp` (located from the test's own path).

## Regression history

- **ANTS-5110:** the two defects above. Locked by this spec.
