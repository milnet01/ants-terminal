# Feature: a child's output is drained as it runs, and a bounded slice kept

`ProcessDrain::run` (`src/processdrain.h`) reads a started `QProcess`'s
stdout while it runs, so `QProcess` never holds more than one read, and
keeps at most `cap` bytes of it. ANTS-6497.

## Invariants

**INV-1 — short output is kept whole.** Output under the cap comes back
unchanged, with `truncated` and `timedOut` false.

**INV-2 — `Keep::Tail` keeps the last bytes.** 3 MB of output ending in
`END`, with a cap of 1000, comes back as exactly 1000 bytes ending in `END`,
with `truncated` true.

**INV-3 — `Keep::Head` keeps the first bytes and lets the child finish.**
Output starting `START`, capped at 1000, comes back as exactly 1000 bytes
starting `START`. The child still runs to its own exit, so its exit code is
the one it chose.

**INV-4 — past the deadline the child is killed.** A child sleeping 30 s,
with a 300 ms deadline, is killed: `timedOut` is true, the call returns
within 5 s, and the process is no longer running.

## Test

`test_process_drain.cpp` in `test_core` runs `/bin/sh -c` children. Memory
use is not measured; INV-2 and INV-3 pin the slice the caller gets.
