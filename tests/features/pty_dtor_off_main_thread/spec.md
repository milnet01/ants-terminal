# Feature: Pty destructor escalation runs off the GUI thread (synchronous on parse-worker)

## Problem

`Pty::~Pty()` (`src/ptyhandler.cpp` lines ~20–89) terminates the child
process via a SIGHUP → close(fd) → WNOHANG cheap-reap → SIGTERM-loop →
SIGKILL ladder. For a shell that exits cleanly on SIGHUP this is
microseconds; for a stubborn shell (`bash -c "trap '' HUP TERM; sleep
1000"`, a wedged interactive shell trapping signals) the SIGTERM loop
takes up to 500 ms before SIGKILL fires.

## Two design eras

### 0.7.33 (problem version): detached `std::thread`

Pre-0.7.0 architecture ran `~Pty` on the GUI thread. With N split
panes closing simultaneously, the GUI serialised through every Pty
destructor — five splits × 500 ms = 2.5 s frozen window, enough for
KWin to throw a "window not responding" hint.

The 0.7.33 fix moved the SIGTERM/SIGKILL escalation onto a detached
`std::thread` so each destructor returned immediately.

### 0.7.0+ (current architecture): `~Pty` already runs on the parse-worker

The threaded-VtStream rework (0.7.0) parents the `Pty` to a `VtStream`
that lives on a per-tab `m_parseThread`. `~VtStream` and `~Pty` run on
that worker thread, not the GUI thread. The GUI's destructor budget
is now bounded by `m_parseThread->wait(2000)` in
`TerminalWidget::~TerminalWidget` — the GUI never blocks on `~Pty`
directly.

## ANTS-1189 — the detached thread now races Qt teardown

User report 2026-05-08: three SIGABRT crashes during normal app exit
(PIDs 3856, 10723, 17749), identical fingerprints — `_int_free_chunk`
unwound from `TerminalWidget::~TerminalWidget` with a smoking-gun
secondary thread parked in `Pty::~Pty()`'s detached lambda inside
`usleep`.

The detached worker outlived the parse-thread's natural termination
and the GUI's `wait(2000)`. By the time the GUI was deep in
`~TerminalWidget` running QPainter / QTextLayout teardown, the
escalation lambda was still alive, racing glibc's malloc arenas
during the worker's eventual `pthread_exit` and surfacing as heap
corruption a few free()s later.

## Fix (0.7.45+): synchronous escalation on the parse-worker

The escalation runs synchronously on the calling thread (which is
already the parse-worker — see `VtStream::~VtStream`). The GUI is
unaffected; the worker thread blocks here, then the GUI's `wait(2000)`
returns when the worker finishes naturally.

```
::kill(m_childPid, SIGHUP);
::close(m_masterFd);
if (waitpid(WNOHANG) == 0) {
    kill(SIGTERM);
    for (i = 0; i < 50; ++i) {
        if (waitpid(WNOHANG) != 0) break;
        usleep(10000);                 // 500 ms cap
    }
    if (waitpid(WNOHANG) == 0) {
        kill(SIGKILL);
        for (i = 0; i < 100; ++i) {    // BOUNDED — was waitpid(0) blocking
            if (waitpid(WNOHANG) != 0) break;
            usleep(10000);             // 1 s cap
        }
    }
}
```

Total destructor budget: ≤ 1.5 s, well within `wait(2000)`. On
multi-pane app exit, panes destruct sequentially on the GUI thread
but the per-pane block lands on the parse-worker via `wait(2000)`,
so worst-case GUI hold is `min(1500, 2000)` per pane × N panes — and
only when every child ignores both SIGHUP and SIGTERM (rare).

## Why bounded SIGKILL reap

Pre-fix code used `waitpid(pid, &st, 0)` after SIGKILL — blocking,
unbounded. Almost always returns within microseconds because SIGKILL
is uncatchable. Edge case: a process in uninterruptible sleep
(broken NFS, wedged kernel I/O) defers signal delivery until kernel
state changes. `waitpid(0)` blocks indefinitely. The worker thread
hangs. `m_parseThread->wait(2000)` times out. The GUI proceeds with
`~QThread` while the underlying thread is still running — Qt
asserts and aborts.

The 1 s bounded loop caps this. On timeout we leave a zombie; init
reaps it on app exit, which is the acceptable degradation path.

## Why no detached thread

- The `Pty` is being destroyed on the parse-worker. There is no
  containing object whose lifetime outlives the destructor for
  storing a joinable `std::thread` member.
- The GUI is already shielded by `m_parseThread->wait(2000)` —
  detaching to "keep the GUI responsive" is solving a problem the
  threaded architecture already solved.
- Detached threads outlive both the calling object and the parse
  thread; their interaction with main-thread Qt teardown is the
  exact race ANTS-1189 fixes.

## Contract

### Invariant 1 — SIGHUP sent up front

`Pty::~Pty` body must `::kill(m_childPid, SIGHUP)` before any
escalation. Most shells exit on SIGHUP within microseconds; without
it the destructor would always escalate to the slower SIGTERM loop.

### Invariant 2 — master FD closed before reap loop

`::close(m_masterFd)` runs before the WNOHANG check. Closing the
master raises SIGHUP on the slave session — second-line trigger for
shells that ignore an explicit kill(SIGHUP) but honor controlling-
terminal hangup.

### Invariant 3 — cheap WNOHANG reap before escalation

`::waitpid(m_childPid, &status, WNOHANG)` is checked before any
SIGTERM. If the child already exited on SIGHUP we skip the
escalation entirely — escalation is the exception path, not the
default.

### Invariant 4 — synchronous SIGTERM/SIGKILL escalation

The destructor body contains both `SIGTERM` and `SIGKILL`. The
SIGTERM loop is bounded at 500 ms (50 × 10 ms `usleep`). The SIGKILL
reap is bounded at 1 s (100 × 10 ms `usleep`).

### Invariant 5 — NO `std::thread` in the destructor

Anti-regression for ANTS-1189. The destructor must not spawn any
`std::thread`, `pthread_create`, `QThread`, or other thread that
outlives the destructor's return. Heap corruption follows from any
detached worker that races main-thread Qt teardown.

### Invariant 6 — NO blocking `waitpid(pid, _, 0)` after SIGKILL

Anti-regression for the worker-hang path. The reap after SIGKILL
must use a bounded WNOHANG loop, never an unbounded blocking
`waitpid` with the third argument literal `0`.

### Invariant 7 — `<thread>` header NOT included

Companion to I5: with no `std::thread` use, the header must not
appear in `src/ptyhandler.cpp`. Keeps the include list a faithful
reflection of what the file actually uses, and prevents a future
edit from accidentally re-introducing the detached-thread path
without anyone noticing the include is "already there".

## How this test anchors to reality

Source-grep on `src/ptyhandler.cpp`. A runtime test would require
forking a stubborn child + measuring main-thread blocking time +
detecting heap corruption — fragile in CI, especially the heap-
corruption signal which only surfaces under specific allocator
state. Source-grep is the reliable signal.

A complementary ASan / TSan build can be added to CI when the user
reports a fresh repro from `build-asan/`. The grep contract here
locks the design; the ASan run would catch any new race a future
edit introduces.

## Regression history

- **Pre-0.7.0:** `~Pty` ran on GUI thread; multi-pane close froze
  the window for N × 500 ms.
- **0.7.0:** threaded VtStream architecture moved `~Pty` onto the
  parse-worker. The freeze became "GUI's `wait(2000)` block" —
  bounded by Qt, no longer unbounded.
- **0.7.33:** detached-thread escalation added (still useful in
  the GUI-bound case if `wait(2000)` is later removed).
- **0.7.45 / ANTS-1189 (2026-05-08):** detached-thread escalation
  removed. The threaded architecture made it redundant; user
  repros showed it produced reproducible app-exit crashes. SIGKILL
  reap also bounded to prevent worker-hang → Qt-abort cascade.
