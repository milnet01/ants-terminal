# Feature: the MCP socket answers an unparsable request

## Problem

`ClaudeIntegration::onMcpConnection` re-parsed the whole buffer on each
`readyRead` and waited while it did not parse. An incomplete request and
a malformed one looked the same to it, so a malformed request got no
JSON-RPC error: the client sat out the 5 s idle timer and was aborted
with no reply. Each re-parse also re-read every byte already buffered
(ANTS-5089).

## Contract

One request per line (user decision, 2026-09-14). A newline completes a
request, and the request is the bytes before the first newline. Bytes
after it are ignored: a connection carries one request.

Until a newline arrives the server does not parse. It scans only the
bytes that just arrived for a newline, and waits, bounded by the 256 KiB
buffer cap and the 5 s idle timer. A request with no newline is never
dispatched, even when it parses.

When the line does not parse, the server replies with JSON-RPC error
`-32700` (Parse error). When it parses but is not an object, the reply
is `-32600` (Invalid Request). Either reply carries `jsonrpc: "2.0"` and
`id: null`, ends in a newline, and closes the connection.

`tools/mcp-bridge.py`, the production client, already ends every request
with a newline.

## Invariants

**INV-1 — a malformed line gets -32700.** Sending `{not json` plus a
newline to a live MCP socket yields a reply whose `error.code` is
`-32700`, with `jsonrpc` `"2.0"` and a null `id`, well before the idle
timer.

**INV-2 — a non-object line gets -32600.** Sending `[1,2]` plus a
newline yields `error.code` `-32600`.

**INV-3 — a request with no newline gets no reply.** A partial request
with no newline gets no reply within 600 ms.

**INV-4 — a complete object with no newline is not dispatched.** An
`initialize` request that parses but has no newline gets no reply within
600 ms. The same request with a newline gets its result.

**INV-5 — a newline ends the request.** A pretty-printed request whose
first line is `{` is answered with `-32700`: that first line is the
whole request.

## Scope

### Out of scope
- The remote-control socket, which already frames by newline.
- The hook socket, which reads until the peer closes and sends no reply.
