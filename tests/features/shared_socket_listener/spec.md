# ANTS-5144 — one socket listener per path, shared by every window

The contract is `docs/specs/ANTS-5144-shared-socket-listener.md`. This test
locks its INV-1 to INV-7. INV-8 is the two existing tests that spec names.

## How it runs

Bare `ClaudeIntegration` and `RemoteControl` instances bind sockets inside a
`QTemporaryDir`, as `tests/features/mcp_async_dispatch/` does. Real clients
connect over those sockets. INV-7 is a source scrape.

## Tests

- `Inv1SecondOwnerBindsNothing` — the socket file's inode survives a second
  start on the same path, and `initialize` still gets a reply.
- `Inv2ServerOutlivesAnyOneOwner` — destroying the first owner, or the newest,
  leaves the path answering.
- `Inv3LivePathIsNeverUnlinked` — `RemoteControl::start` refuses a path a
  plain server accepts on, and replaces a socket file nobody accepts on.
- `Inv4ConnectionServedByMostRecentlyActivatedVisibleOwner` — each owner
  registers one verb name returning its own label; activation order,
  visibility and a newly attached owner decide which label comes back.
- `Inv5HookEventReachesOneOwner` — `hookEventsProcessedForTest()` rises on the
  owner whose session probe claims the event, else on the last activated.
- `Inv6DestroyingAnOwnerClosesItsConnections` — the socket serving a deferred
  verb is a direct child of its owner, and destroying the owner deletes it and
  disconnects the client.
- `Inv7AcceptChecksStayInTheHandlers` — `SO_PEERCRED` and the 5 s idle timer
  precede `readyRead` in each handler; the `attach` calls serve through those
  handlers; the hub pulls a connection only to close it.

## ANTS-5138 — the client waits for a slow first byte

`RemoteControl::runClient` waits for the first byte of a reply for up to its
overall read deadline, then keeps the 2 s wait between later chunks. A server
that answers 2.5 s after the request is read, and the client exits 0. Bytes
that start but do not finish by the deadline still end in the timed-out
refusal. *Test:* `SharedSocketListener.Ants5138SlowFirstByteIsStillReceived`.
