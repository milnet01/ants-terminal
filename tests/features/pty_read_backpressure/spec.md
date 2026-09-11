# Feature: the PTY read loop honours back-pressure and never double-reaps at EOF

## Invariants

**INV-1 — a pause requested inside a `dataReceived` handler is honoured at
once.** `Pty::setReadEnabled(false)`, called from inside a slot connected to
`dataReceived`, must stop `onReadReady`'s read loop before it emits another
chunk. The notifier is level-triggered, so calling `setReadEnabled(true)`
later resumes delivery.

**INV-2 — `finished` fires at most once per child.** Once `onReadReady` has
processed EOF and emitted `finished`, a later `setReadEnabled(true)` must not
cause a second `finished` emission for the same child.

**INV-3 — reaping after EOF never touches an unrelated process.** Once a
child has been reaped, any later reap attempt triggered by the read loop
must not call `waitpid` with a pid that matches "any child" — doing so can
reap a process the `Pty` object never started.

## Rationale

`Pty::onReadReady` reads in a loop until `EAGAIN`. `VtStream` asks the `Pty`
to pause by disabling the read notifier from inside a `dataReceived` handler
when too many parse batches are in flight. Disabling the notifier stops it
from firing again, but does not stop a read loop already running — so the
loop keeps reading and emitting after the pause was requested. Every one of
those emissions grows `VtStream`'s pending-actions queue, and the next ack
ships the whole backlog as one batch the GUI applies in one pass; while that
runs, queued keystrokes (Ctrl+C included) cannot reach the PTY, and a
blocking-queued resize freezes the GUI thread. A fast producer — `yes`, a
large log, a runaway build — triggers this on an ordinary terminal, not only
under adversarial input.

The same loop shape opens a second hole at EOF. Nothing marks the child as
already reaped in a way the loop itself checks, so a later
`setReadEnabled(true)` can re-enter the EOF branch a second time. On that
second pass the `Pty`'s own child is gone, so the `waitpid` call reaps
whichever process the kernel hands back for "any child" — which may belong
to someone else entirely.

## Why behavioural

The claim is about what actually happens on the wire and in the process
table, not about the shape of the source. A grep can confirm a `break`
statement exists; it cannot show that a chunk still arrives after the pause
was requested, that a signal fires twice, or that a `waitpid` call reaped
the wrong pid. Only driving a real `Pty` against a real child process, with
a real event loop pumped in between, can show any of the three.

## Scope

### In scope
- `Pty::onReadReady`'s read-loop and EOF-handling behaviour, observed
  through `Pty`'s public signals (`dataReceived`, `finished`) and through
  `waitpid` calls the test makes itself.

### Out of scope
- `VtStream`'s batching / ack logic — a separate contract, exercised
  elsewhere. This spec only pins what `Pty` itself does with the read
  notifier and with `waitpid`.
- The write side (`Pty::write` / `onWriteReady`) — covered by
  `tests/features/pty_write_eagain_queue`.
- Any claim about how many bytes a Linux PTY master read returns. The
  test asserts one chunk after the pause and names no chunk size.

## Regression history

- **ANTS-5026:** `Pty::onReadReady`'s read loop does not check whether the
  read notifier has been disabled, so a pause requested from inside a
  `dataReceived` handler takes effect only after the loop drains whatever
  is already queued in the kernel. The same loop shape lets a second
  `setReadEnabled(true)` after EOF re-enter the EOF branch and reap an
  unrelated child via a bare "any child" `waitpid`. Fix: break once the
  notifier is disabled, and never read or reap again once EOF has been
  processed. No separate read budget: acks reach the worker only through
  its event loop, so the loop pauses itself once `VtStream`'s in-flight cap
  is reached. Locked by this spec.
