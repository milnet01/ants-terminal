# Feature: PTY write queues on EAGAIN, drains via write-side notifier

## Contract

When `Pty::write` issues `::write(m_masterFd, …)` and the kernel returns
`EAGAIN` (the master's PTY-side buffer is full), the unwritten remainder
of the caller's payload MUST be queued on `m_pendingWrite` and drained
later via a `QSocketNotifier(QSocketNotifier::Write)` that fires when
the kernel signals the FD is writable again.

The previous implementation broke out of the write loop on EAGAIN with
no buffering — the caller's bytes were silently dropped. Even moderate
output bursts to a slow consumer (tarball pasted into a shell, AI-dialog
template injection, plugin-driven keystrokes during a `find /` flood)
could lose user data with no diagnostic.

## Rationale

The master FD is non-blocking (`O_NONBLOCK` set in `Pty::start`) so a
slow reader on the slave side cannot block the GUI thread. The
trade-off is that `::write` returns short — partially writing the
payload then returning EAGAIN once the kernel buffer fills. POSIX
correct behaviour for non-blocking I/O is "queue the remainder, install
a writability poller, drain on the next signal." That's what every
production Qt-based terminal does (Konsole's `Pty::sendData` uses
`KPtyDevice::write` which is itself a `QIODevice` with a Qt-managed
write buffer; gnome-terminal's vte uses a `GIOChannel` with a write
queue; xterm uses select() on the master FD).

A bounded queue is required: if the slave never reads (stuck child,
suspended job, paused stdin), the queue would otherwise grow without
bound until the GUI process OOMs. Cap at 4 MiB — large enough that
realistic burst writes (≤ 1 MiB pastes, AI command insertions) fit
without truncation; small enough that pathological behaviour does not
exhaust process memory.

FIFO ordering is mandatory: a fresh `write()` issued while bytes are
still pending must NOT race ahead of those pending bytes. The flow
is "drain queue first, then accept new writes" — implementations
must check `m_pendingWrite` before attempting any direct `::write`.

## Invariants

**INV-1 — `QSocketNotifier::Write` is created on `m_masterFd`.**
Source-grep against `src/ptyhandler.cpp`: a `new QSocketNotifier(`
construction site referencing `QSocketNotifier::Write` must appear
inside `Pty::start`, post-`m_readNotifier` setup. Without it, drain
on EAGAIN cannot be triggered.

**INV-2 — `m_pendingWrite` queue member exists in header.**
Source-grep against `src/ptyhandler.h`: a `m_pendingWrite` member
of byte-buffer type (`QByteArray` / `std::string`) must be declared.

**INV-3 — `m_writeNotifier` pointer member exists in header.**
Source-grep: `m_writeNotifier` declared as `QSocketNotifier *`.

**INV-4 — `onWriteReady` (or equivalent) slot exists and is connected.**
Source-grep `src/ptyhandler.h` for an `onWriteReady` slot declaration
and `src/ptyhandler.cpp` for a `connect(…, &Pty::onWriteReady)`
binding. Slot name MAY differ (e.g. `drainWriteQueue`) provided the
connection target appears.

**INV-5 — `Pty::write` handles `EAGAIN` distinctly from fatal errors.**
The pre-fix code's else-clause comment was `// EAGAIN or fatal error`
— a single break path that conflated recoverable back-pressure with
real failure. Source-grep: an `EAGAIN` token must appear inside the
`Pty::write` function body. The implementation must explicitly branch
on EAGAIN to queue, not silently drop.

**INV-6 — Queue capacity is bounded by a numeric cap.** Source-grep:
a numeric constant (e.g. `MAX_PENDING_WRITE_BYTES`, `4 * 1024 * 1024`,
or `4 << 20`) must appear in `ptyhandler.{h,cpp}`. The exact cap value
is implementation choice; the invariant is that *some* cap exists.

**INV-7 — Direct-write path checks the queue first.** A `write()`
issued while `m_pendingWrite` is non-empty must append to the queue,
never bypass it. Source-grep: the `Pty::write` body must contain a
test of the form `m_pendingWrite.isEmpty()` (or equivalent
`empty()`/`!size()` check) so fresh writes are queued behind pending
ones.

## Scope

### In scope
- Source-grep regression test pinning the queue + notifier + cap +
  FIFO-preservation invariants.

### Out of scope
- Behavioral test that floods a slow consumer and asserts the GUI's
  buffer accumulates rather than drops. Would need a paired
  pseudo-terminal where the slave deliberately stops reading; the
  Qt event loop must run; non-trivial harness for a structural fix.
- Per-write coalescing / Nagle-style batching. Distinct optimization;
  this fix preserves the call-per-write API surface.
- Backpressure feedback to the caller (e.g. emitting `writeStalled`
  signals when the queue fills). Future enhancement; this fix
  preserves the silent-drop semantics on overflow but adds a debug
  log line so the situation is observable.

## Regression history

- **Original implementation:** `else { break; // EAGAIN or fatal error }`
  in `Pty::write` — kernel back-pressure equated with terminal failure,
  silent data loss for any caller exceeding the master's PTY buffer in
  one syscall. Behaviourally the loss only became user-visible during
  large pastes / AI-suggested command insertions / plugin-driven
  keystroke bursts, which is why the bug had a long shelf life.
- **0.7.12 ROADMAP entry (Tier 2 hardening):** flagged as a data-loss
  bug; recommended `QSocketNotifier(QSocketNotifier::Write)` + queue.
- **0.7.27 (this fix):** queue + write notifier + 4 MiB cap. Locked
  by this spec.
