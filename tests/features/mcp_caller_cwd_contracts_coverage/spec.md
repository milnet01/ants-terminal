# Feature spec: every registered MCP tool has an explicit CallerCwdContract entry (ANTS-1417)

`ClaudeIntegration::callerCwdContractFor(toolName)` returns
`Optional` as the catch-all default — safe but silent. A new tool
added via `registerToolProvider("foo", ...)` automatically falls
through to Optional without anyone noticing, even when the tool
mutates per-project state and should refuse on empty `caller_cwd`
(per ANTS-1404 contract enforcement). This test asserts every
registered tool has an explicit `if (toolName == "xxx") return …`
branch in `callerCwdContractFor`.

## Invariants

- **INV-1 / every registered tool is explicitly classified.** For
  every `m_claudeIntegration->registerToolProvider("<name>", …)`
  call in `src/mainwindow.cpp`, there MUST be a matching
  `if (toolName == QStringLiteral("<name>"))` branch in
  `ClaudeIntegration::callerCwdContractFor()` in
  `src/claudeintegration.cpp`. A tool not in `callerCwdContractFor`
  silently falls to `Optional`, which is fine for some tools but
  hides the audit decision.
- **INV-2 / get_session_info is exempt.** ANTS-1404's existing
  carve-out — `get_session_info` is dispatched inline (not via
  `registerToolProvider`) so it does not appear in the registered-
  tool list. The test scope is the union of registered tools,
  not the dispatcher's full tool surface.
- **INV-3 / tool_info is exempt.** Same rationale —
  `tool_info` is one of the inline-dispatched verbs (ANTS-1399).
  Its classification stays in `callerCwdContractFor` for symmetry
  but the registration-coverage test doesn't gate on it.

## Test scope

Source-scrape against `src/mainwindow.cpp` (registerToolProvider
calls) and `src/claudeintegration.cpp` (callerCwdContractFor
branches). Pure regex; no GUI / RemoteControl / dispatcher
instantiation needed. Failure mode the test catches: a future
contributor adds a new tool, forgets to classify it, and the
behavior silently defaults to Optional.
