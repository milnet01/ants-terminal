# Feature: Pty destructor escalation runs off the main thread

## Problem

Pre-fix `Pty::~Pty()` (at `src/ptyhandler.cpp` lines 36-53)
escalated child reaping in three steps, all on the calling
thread:

1. Send SIGHUP, then `waitpid(WNOHANG)` — fast, fine.
2. If still running, send SIGTERM and busy-wait
   `usleep(10ms) + waitpid(WNOHANG)` up to 50 times = up to
   500 ms.
3. If still running, SIGKILL + blocking `waitpid(0)`.

For one shell that exits cleanly on SIGHUP this is microseconds.
For a stubborn shell — most commonly a `bash -c "trap '' HUP TERM;
sleep 1000"` test, or a wedged interactive shell trapping signals
— the destructor blocked the GUI for the full 500 ms (worst case
500 ms + the SIGKILL wait).

With N split panes closing simultaneously when the main window
closes, the GUI thread serialises through every Pty destructor.
Five splits × 500 ms = 2.5 s frozen window. Enough for KWin to
throw a "window not responding" hint.

## Fix

Move the SIGTERM-wait-SIGKILL escalation to a detached
`std::thread` constructed inside the destructor:

```
const pid_t pid = m_childPid;
std::thread([pid]() {
    int st = 0;
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        if (::waitpid(pid, &st, WNOHANG) != 0) return;
        ::usleep(10000);
    }
    if (::waitpid(pid, &st, WNOHANG) == 0) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &st, 0);
    }
}).detach();
```

The destructor body is now: SIGHUP → close fd → cheap WNOHANG
reap → spawn-and-detach worker → return. Window-close is now
microseconds per pane regardless of how stubborn the children
are. The detached thread reaps the child on its own; if Ants
itself exits before the worker finishes, the kernel adopts the
child via init and reaps the worker thread cleanly.

## Why detached, not joined / member

- The Pty object is being destroyed — there's no place to store
  a joinable `std::thread` member that outlives the object.
- The worker captures only `pid` (by value), no `this`. Safe.
- Crash / SIGKILL of Ants leaves no leak: the kernel adopts
  the orphan child, the worker thread evaporates with the
  process. Not strictly graceful, but no different from what
  happens if Ants segfaults during a synchronous destructor.

## Fallback

Thread creation can fail under `ulimit -u` pressure at exit
time. The fix wraps the `std::thread` constructor in a
try/catch and falls back to the synchronous escalation —
better a 500 ms freeze than a leaked child / zombie.

## Contract

### Invariant 1 — destructor includes a `std::thread([` lambda

`Pty::~Pty` body must contain `std::thread([pid]()` (or close
variant) — the marker for "escalation moved off thread."

### Invariant 2 — detached, not joined

The same body must contain `.detach()` immediately following
the lambda construction. Joining would block the destructor
and re-introduce the freeze.

### Invariant 3 — pid captured by value

The lambda capture list must be `[pid]` (or any explicit
by-value capture of the local pid copy). A `[this]` or `[&]`
capture would dangle on Pty destruction.

### Invariant 4 — `<thread>` header included

`src/ptyhandler.cpp` includes `<thread>`. Without this the
compile breaks on systems where `<thread>` isn't transitively
available through other headers (varies by libstdc++ /
libc++ version).

### Invariant 5 — synchronous fallback retained

A `try { ... }.detach(); } catch (const std::system_error &)`
block must surround the thread construction. The catch body
contains the SIGTERM/usleep loop + SIGKILL fallback so the
escalation still completes when thread creation fails.

### Invariant 6 — pre-escalation cheap reap retained

The destructor still does `kill(SIGHUP) → close fd →
waitpid(WNOHANG)` BEFORE deciding to escalate. Spawning a
thread for every PTY close, even ones that exit cleanly on
SIGHUP, would be wasteful.

## How this test anchors to reality

Source-grep on `src/ptyhandler.cpp`. A runtime test would
require forking a stubborn child + measuring main-thread
blocking time, which is fragile in CI. Source-grep is the
reliable signal.

## Regression history

- **Latent:** every Ants version since `Pty::~Pty` shipped.
- **Flagged:** ROADMAP "PTY dtor off-main-thread."
- **Fixed:** 0.7.33 — Lifecycle/cleanup bundle.
