---
paths:
  - "src/mcp*"
  - "src/trace/**"
  - "src/claudeintegration*"
  - "src/mainwindow_mcp.cpp"
  - "src/remotecontrol*"
  - "docs/standards/mcp-*.md"
---

## Where a verb is registered (hot reload)

- A verb that needs no tab or terminal state is registered in
  `mcp::registerProjectScopedVerbs()` (`src/mcptoolregistry.cpp`), or in
  `mcp::registerMcpdOnlyVerbs()` (`src/trace/traceverb.h`) when only
  `ants-mcpd` serves it. Its body
  reaches `MainWindow` in none of ANTS-4932 § 1's three forms: a direct call,
  a call inside the root resolver, or an `if (!m_main)` refusal.
- A verb that needs that state names its `MainWindow` method in its design
  and joins `mcp::terminalScopedVerbNames()`, which gives `ants-mcpd` its
  forwarder.
- `CallerCwdContract::TabSpecific` is not that register. It covers per-tab
  reads routed by `tab` or `caller_cwd`; adding a verb to it switches on a
  refusal.

## Verb references

- Per-verb notes:
  [`docs/standards/mcp-behavioural-notes.md`](../../docs/standards/mcp-behavioural-notes.md).
- Config keys, including the master gate `claude.mcp_enabled`:
  [`docs/standards/mcp-config-keys.md`](../../docs/standards/mcp-config-keys.md).
