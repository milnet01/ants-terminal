# Feature: the PTY child PID is safe to read from the GUI thread

## Problem

`Pty` lives on a per-tab parse worker thread, moved there by
`TerminalWidget`. `TerminalWidget::ptyChildPid` reads
`Pty::childPid()` from the GUI thread, and `terminalwidget.h`
justified that with:

> the PID is written once during forkpty() (synchronised by
> startShell's BlockingQueuedConnection) and never changes afterwards.

That is not true. `Pty::onReadReady` is the read-notifier slot and runs
on the worker thread. When the child is reaped at EOF it writes
`m_childPid = -1`. The destructor writes it too. So the member is
written on the worker thread and read, unsynchronised, on the GUI
thread — a data race, and the comment asserting otherwise is what stops
a reader noticing.

The consequence is not theoretical. Two GUI-thread callers use the
value to open `/proc/<pid>`: `openFileAtPath` resolves a relative path
against the shell's working directory, and `foregroundProcess` reads
the foreground process group. A read that misses the clear returns a
PID the kernel may already have recycled, so both can consult an
unrelated process. Neither signals the PID, so this is a
wrong-answer bug rather than a wrong-target one.

## Contract

`m_childPid` is written on the worker thread and read on the GUI
thread, so it MUST be an atomic. `pid_t` is an `int`; the atomic is
lock-free, and every existing use — comparison, `kill`, `waitpid`,
assignment — works unchanged through it.

Atomicity is the whole of the fix. It removes the race and guarantees
the reader eventually sees the clear. It does not remove the inherent
gap between reading a PID and using it: the child can exit at any
moment regardless, and `/proc/<pid>` simply fails when it has. That
gap is not closable and is not what this feature claims.

The comment in `terminalwidget.h` MUST stop claiming the PID never
changes after `forkpty`.

## Invariants

**INV-1 — `m_childPid` is declared atomic.** Source-grep
`src/ptyhandler.h`: the member is an `std::atomic` over `pid_t`.

**INV-2 — no plain declaration remains.** A non-atomic `pid_t
m_childPid` declaration must not be present, so the fix cannot be
half-reverted into a second, plain member.

**INV-3 — `<atomic>` is included.** Relying on a transitive include
for the type in a public header is how INV-1 breaks on another
toolchain.

## Scope

### In scope
- Source-grep over `src/ptyhandler.h`.

### Out of scope
- A test that actually races the two threads. It would be
  non-deterministic by construction, and a green run would prove
  nothing; ThreadSanitizer is the tool for that class and the project
  does not currently run it. The declaration is what the fix is, and
  it is what this pins.
- The read-then-use gap described above. Not closable.
- `m_masterFd`, which is written on the same paths. It is only ever
  read on the worker thread, so it is not part of this claim; a
  GUI-thread reader appearing later would need the same treatment.

## Regression history

- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "`m_childPid` written on worker thread, read unsynchronised on GUI
  thread against a comment asserting it never changes". Verified
  against source — the write is in `onReadReady`'s reap branch — and
  fixed. Locked by this spec.
