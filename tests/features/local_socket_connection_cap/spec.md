# Feature: the Claude hook and MCP sockets cap concurrent connections

## Problem

`ClaudeIntegration::onHookConnection` and `onMcpConnection` accept every
connection a same-UID peer opens. Each one may buffer up to 256 KiB for up
to the 5 s idle timer, and nothing bounds how many are open at once, so
memory held by open connections is unbounded (ANTS-5089).

## Contract

Each `ClaudeIntegration` admits at most
`ClaudeIntegration::kMaxLiveConnections` live connections per server. A
connection past the cap is disconnected on accept, before anything is read
from it. A connection counts until its socket is destroyed, so a slot is
free again once an admitted connection has closed.

## Invariants

**INV-1 — the MCP socket refuses past the cap.** With
`kMaxLiveConnections` idle clients connected, one more client is
disconnected within 1 s while the first client is still connected.

**INV-2 — the hook socket refuses past the cap.** The same, against the
hook socket.

**INV-3 — a slot frees when a connection closes.** After INV-1's refusal,
closing one admitted client lets a new client connect and stay connected.

## Scope

### Out of scope
- The remote-control socket, which has its own accept path.
