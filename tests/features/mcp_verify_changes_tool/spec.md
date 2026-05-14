# ANTS-1289 — verify_changes MCP wiring conformance

Region-scoped source-grep test. Mirrors `mcp_debt_sweep_tools` and
`mcp_indie_review_tools`. Asserts the `verify_changes` tool is
registered consistently across all four layers:

1. **claudeintegration.cpp** — tool name + inputSchema block.
2. **mainwindow.cpp** — `registerToolProvider("verify_changes", ...)`.
3. **remotecontrol.h** — `cmdVerifyChanges(...)` declaration.
4. **remotecontrol.cpp** — `RemoteControl::cmdVerifyChanges(...)` definition.

## Invariants covered

- **REG-1** — tool name `"verify_changes"` appears in
  claudeintegration.cpp.
- **REG-2** — `registerToolProvider("verify_changes",` exists in
  mainwindow.cpp.
- **REG-3** — `cmdVerifyChanges` is declared in remotecontrol.h.
- **REG-4** — `RemoteControl::cmdVerifyChanges` is defined in
  remotecontrol.cpp.
- **REG-5** — the tool's inputSchema block sets
  `additionalProperties: false` (verifies INV-7 from the engine spec
  at the registration layer).
- **REG-6** — the optional args (`gates`, `max_log_lines`,
  `timeout_sec`) appear in the schema's properties{} block.

## Region scoping

This test uses the `// ANTS-1289` comment anchor as its region
delimiter. Future tools registered after `verify_changes` should
introduce their own `// ANTS-NNNN` anchor so this scan terminates
cleanly at the family boundary.
