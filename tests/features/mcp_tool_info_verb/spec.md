# mcp_tool_info_verb — ANTS-1399

See `docs/specs/ANTS-1399.md`.

## Test scope

Source-scrape regression locks the `tool_info` MCP verb's
schema, classification, dispatch path, and the error-envelope
codes the handler emits.

## Invariants checked

- **INV-1.** `tool_info` registered as an MCP tool in
  `claudeintegration.cpp`'s `tools/list` block with `name`
  required and `additionalProperties: false`.
- **INV-2.** `tools/list` end-of-build site stores `tools`
  into `m_lastToolsList` (anchor: `ANTS-1399-INV-2`).
- **INV-3.** Handler scans `m_lastToolsList` and emits the
  per-tool `{name, description, inputSchema}` slice
  (anchor: `ANTS-1399-INV-3`).
- **INV-4.** Unknown-name path emits `code:"unknown_tool"`
  with an `available` array.
- **INV-5.** Missing-name path emits `code:"missing_name"`.
- **INV-6.** Cold-snapshot path emits
  `code:"tools_not_ready"`.
- **INV-7.** `callerCwdContractFor` classifies `tool_info`
  as `ProcessGlobal`.
- **INV-8.** Handler dispatched inline alongside
  `get_session_info`, not via `m_toolProviders`.
