# Feature: the MCP socket answers an unparsable request

## Problem

`ClaudeIntegration::onMcpConnection` re-parses the whole buffer on each
`readyRead` and waits while it does not parse. An incomplete request and
a malformed one look the same to it, so a malformed request gets no
JSON-RPC error. The client sits out the 5 s idle timer and is aborted
with no reply. `tools/mcp-bridge.py` answers malformed JSON from its own
client itself, so this reaches only a direct socket client.

## Contract

The server keeps accepting unframed requests: a buffer that parses as a
JSON object is dispatched whether or not it ends in a newline.

A buffer that is exactly one line — its only newline is the last byte —
is a complete request. If that line does not parse, the server replies
with JSON-RPC error `-32700` (Parse error). If it parses but is not an
object, the reply is `-32600` (Invalid Request). Either reply carries
`jsonrpc: "2.0"` and `id: null`, ends in a newline, and closes the
connection.

Any other buffer that does not parse is a request still arriving, and
the server keeps waiting as before. That covers a buffer with no
newline, and one with a newline before its end, such as a pretty-printed
request split across writes.

## Invariants

**INV-1 — a malformed line gets -32700.** Sending `{not json` plus a
newline to a live MCP socket yields a reply whose `error.code` is
`-32700`, with `jsonrpc` `"2.0"` and a null `id`, well before the idle
timer.

**INV-2 — a non-object line gets -32600.** Sending `[1,2]` plus a
newline yields `error.code` `-32600`.

**INV-3 — a request in progress gets no reply.** A buffer with no
newline, and a buffer with an embedded newline, each get no reply within
600 ms.

## Scope

### Out of scope
- Ending the whole-buffer re-parse on each `readyRead`. That needs
  newline framing to be required, which would break unframed direct
  clients (`tests/features/mcp_async_dispatch` sends none).
- The remote-control socket, which already frames by newline.
