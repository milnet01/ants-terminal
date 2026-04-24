# Feature: Process-wide SIGPIPE ignore in main()

## Contract

`ants-terminal` MUST install `SIG_IGN` for `SIGPIPE` before any code path
that can `write()` to a file descriptor the kernel might have closed
(notably: PTY master FDs whose slave just reaped its child shell). The
ignore must be installed in `main()` before `QApplication` is
constructed.

Without this disposition, `write(pty_fd, buf, n)` to a just-closed PTY
delivers `SIGPIPE` to the GUI process, and the default handler
terminates ants-terminal with no diagnostic — a full-window crash
triggered by ordinary shell-exit timing.

## Rationale

`ptyhandler.cpp` calls `::write()` on the PTY master. The PTY slave can
close at any instant (shell exited, kernel reaped, FD torn down).
Between the moment our `QSocketNotifier` fires on the next write and
the moment the `write()` returns `EPIPE`, the kernel has already
queued `SIGPIPE`. Default disposition is terminate; we need that delivery
to be a no-op so we observe `errno == EPIPE` via the return code path
and handle it cleanly.

This is a process-wide invariant — the signal disposition is inherited
by any thread the process spawns. We do NOT set it per-thread and we do
NOT rely on `MSG_NOSIGNAL` (only works for sockets, not character
devices, and is a per-call flag that's easy to miss in a new caller).

## Invariants

**INV-1 — `std::signal(SIGPIPE, SIG_IGN)` is called in main().**
Source-grep against `src/main.cpp`: a call of the form
`std::signal(SIGPIPE, SIG_IGN)` (or the less-portable `signal()` without
the `std::` prefix) must appear inside the `int main(…)` function body.

**INV-2 — The call is the first statement after `main()`'s opening
brace, before `QApplication` constructor.**
If a write path fires from a static initializer (e.g. plugin-manager
auto-registration touching a socket), Qt's resource loading, or
anything else that runs before main's first line, the ignore would be
too late. The call must appear textually before any `QApplication app`
or `QCoreApplication app` instantiation.

**INV-3 — `<csignal>` is included.**
`std::signal` and `SIG_IGN` are declared in `<csignal>`. The include
must be present so the code compiles on libc implementations that
do not pull the declarations in via a transitive header.

## Scope

### In scope
- Source-grep regression test pinning the call site.

### Out of scope
- Behavioral test that raises SIGPIPE and confirms no termination.
  Would require spawning a subprocess under a chosen Qt platform; the
  invariant is structural (the call exists + precedes QApplication)
  and that is sufficient for regression safety.
- Per-thread signal masks (we rely on the process-wide default).
- Windows (not supported).

## Regression history

- **0.6.28 (bc97485):** Original fix in the fifth-audit batch. Never had
  a dedicated regression test — listed as outstanding in ROADMAP § Tier
  2 until 0.7.17, at which point we discovered it was already shipped
  and locked the source-grep invariant to prevent a quiet removal.
