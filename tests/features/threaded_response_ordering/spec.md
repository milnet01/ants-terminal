# Threaded response ordering

**Status:** locked in 0.7.0.

## Invariant

When the grid fires its `ResponseCallback` during `processAction` in
response to PTY input (DA1, DA2, CPR, DSR, OSC 52 read, Kitty kbd query,
etc.), the callback fires **in the order the parser consumed the input
sequences**.

Under 0.7.0's threaded parse path this order matters for a subtle reason:
when `ResponseCallback` routes bytes back to `Pty::write` via an
`invokeMethod(... QueuedConnection)` hop, Qt's per-receiver FIFO event
queue preserves that order across the thread boundary. So **if the grid
preserves order at the callback layer, end-to-end write order is
preserved too**. This test pins the grid-layer invariant; the Qt
queue layer is provider-guaranteed.

## What we test

Feed a byte stream containing, in order:

1. `ESC[c`   → DA1 response
2. `ESC[6n`  → CPR response
3. `ESC[>c`  → DA2 response
4. `ESC[?996;1n` → DEC report for mode 996 (color scheme)
5. `ESC[5n` → DSR status report

through a real `VtParser` + `TerminalGrid`, with a response-callback
that records every response in arrival order. Assert the recorded
vector is exactly `[DA1, CPR, DA2, color-scheme, DSR-OK]`.

Then: feed the same stream split into 1-byte chunks. Assert identical
order (chunking must not reorder — already covered structurally by
`threaded_parse_equivalence`, but this test pins it end-to-end on the
response side).

## Scope

Source-level unit test against `TerminalGrid` + `VtParser`. No Qt widgets,
no QThread. The contract this locks down is scoped to the grid layer;
the Qt cross-thread hop is a separate, provider-tested layer.
