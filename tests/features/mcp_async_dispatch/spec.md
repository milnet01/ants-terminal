# ANTS-2132 — MCP verbs dispatch off the GUI thread

## Background

`ClaudeIntegration::onMcpConnection()` served every MCP request on the thread
that owns the widgets: the `readyRead` handler called the tool handler inline
and ran the whole response pipeline before returning to the event loop. So the
Ants window could not repaint for a verb's full duration, and with several
Claude sessions live their verbs serialised through that one thread — which is
why the freezes the user reported had no pattern they could see.

The full contract is `docs/specs/ANTS-2132-async-mcp-dispatch.md`. This file
covers what *this* test locks.

## Why this test is behavioural, not a source scrape

Every other test around this change reads `mainwindow.cpp` for a factory
spelling. A scrape cannot tell whether a verb actually leaves the GUI thread,
and that is the entire claim. This one stands up a real `ClaudeIntegration`,
binds an MCP socket in a `QTemporaryDir`, drives `tools/call` over it from a
`QLocalSocket`, and observes which thread the handler ran on.

The harness did not exist before this test — no other test in the suite
constructs a `ClaudeIntegration`. It works because `startMcpServer()` takes an
explicit socket path and the GUI bundle's `main` already builds a
`QApplication` under `QT_QPA_PLATFORM=offscreen`.

## Contract

- **INV-1** — a verb registered through the `RcHandler` overload with a
  non-`TabSpecific` contract does not run on the GUI thread, and the GUI thread
  keeps processing events while it runs.
- **INV-2** — off-thread verbs execute one at a time. One worker, not one per
  call, so no pair of off-thread verbs begins to overlap.
- **INV-4** — a `TabSpecific` verb stays on the GUI thread however it was
  registered. It reads live terminal state through `MainWindow`.
- **INV-5** — a handler registered through the bare `ToolHandler` overload
  never runs off the GUI thread. That overload is what every hand-written
  inline lambda in `mainwindow.cpp` uses, and those capture `MainWindow`.

Numbering follows the parent spec's, so a reader can move between them without
a mapping table.

## The red run, and what it measured

Verified by forcing `offThread = false` in
`ClaudeIntegration::registerToolProvider`'s `RcHandler` overload — which is
exactly the pre-change behaviour — rebuilding, and re-running:

```
the verb ran on the GUI thread — dispatch is still synchronous
the GUI thread was not processing events while the verb ran (ticks=1)
[  FAILED  ] McpAsyncDispatch.Inv1EligibleVerbLeavesTheGuiThreadAndItKeepsPainting
```

**`ticks=1` is the reported bug, measured.** A 10 ms heartbeat timer fired
exactly once across a 200 ms verb, because the GUI thread was inside the
handler for all of it. With the fix the same timer fires ~20 times.

## Known limits of this test

- **INV-2 does not discriminate in the red run.** Under synchronous dispatch
  nothing overlaps either, so it passes for the wrong reason. It is kept
  because it guards a *future* change — moving to a thread per call would fail
  it — not because it fails today without the fix.
- **INV-8 and INV-10 of the parent spec are not covered here.** The
  disconnect-mid-verb and queue-cap cases need a client that can abandon a
  request mid-flight and 65 concurrent sockets; the rate limiter also
  interferes with the second. Tracked rather than silently dropped.

## Test

`test_mcp_async_dispatch.cpp`, built into the `test_claude` bundle. Label
`features;fast`. It binds a socket per test in a `QTemporaryDir`, so runs do
not collide and nothing is left behind.
