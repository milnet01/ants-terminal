# Feature: a remote-control reply nobody reads does not pin the socket

## Problem

`RemoteControl::onNewConnection` stops the 5 s idle timer before it
dispatches a request. The reply write then runs `write`, `flush` and
`disconnectFromServer`. `disconnectFromServer` waits for the write
buffer to drain before it closes. A peer that never reads a large reply
(`get-text` allows 16 MiB) leaves the socket in the closing state for
good, holding the reply in memory. Nothing times it out.

## Contract

After a reply is written, the connection MUST be aborted if the peer
makes no read progress for `RemoteControl::kReplyDrainIdleMs` (30 s).
Progress is measured by `bytesWritten`: each one restarts the window, so
a slow reader that keeps reading is never cut.

`RemoteControl::armReplyDrainGuard(socket, idleMs)` implements the rule.
The reply write in `onNewConnection` calls it after
`disconnectFromServer`. It does nothing to a socket that has already
closed.

## Invariants

**INV-1 — a stalled reader is aborted.** A bare `QLocalServer` writes a
reply far larger than the kernel socket buffer to a raw `AF_UNIX` client
that never reads, calls `disconnectFromServer`, then arms the guard with
a short window. The server socket MUST reach the unconnected state.

**INV-2 — a draining reader is not cut.** The same setup with a client
that reads the reply in small chunks, each sooner than the window, MUST
receive every byte.

**INV-3 — the reply write arms the guard.** Source-grep: the reply
write in `RemoteControl::onNewConnection` calls
`armReplyDrainGuard(` with `kReplyDrainIdleMs` after
`disconnectFromServer()`.

## Scope

### Out of scope
- The MCP and hook servers. ANTS-5089 reports the same gap there.
- A cap on live connections. Since ANTS-5144 the three servers share
  `LocalSocketHub`, so a cap belongs there and changes
  `docs/specs/ANTS-5144-shared-socket-listener.md`.
