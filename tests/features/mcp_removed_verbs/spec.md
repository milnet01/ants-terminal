# Removed MCP verbs say what replaced them (ANTS-5485)

The twenty verbs deprecated in 0.7.112 are removed. A session that still
calls one is told what replaced it. The full contract is
`docs/specs/ANTS-5485-retire-deprecated-verbs.md`; this test covers its
INV-2, INV-3, INV-4 and INV-7's registry half.

## Invariants

- **INV-2** — `mcp::removedVerbError(v)` for each removed verb is
  `{code:-32602, message:"Tool <v> was removed (ANTS-5485); use <r> instead.",
  data:{code:"verb_removed", replacement:<r>}}`, where `<r>` is
  `mcp::removedReplacement(v)` and is not empty. `finishToolDispatch`'s
  unknown-tool branch calls it before falling back to the generic message.
  *Test:* `Inv2RemovedVerbErrorShape`, `Inv2DispatchUsesIt`.
- **INV-3** — a name that was never a verb gets an empty error object and an
  empty replacement, so the dispatch keeps `Unknown tool: <name>`.
  *Test:* `Inv3OtherNamesUnchanged`.
- **INV-4** — `tool_info`'s `unknown_tool` refusal consults
  `mcp::removedReplacement`. *Test:* `Inv4ToolInfoNamesReplacement`.
- **INV-7 (registry half)** — the nine kept verbs that share a prefix or a
  handler with a removed one are still registered, no removed verb is, and
  `tools/list` lists none of the twenty. *Test:* `Inv7KeptVerbsRegistered`,
  `Inv7RemovedVerbsNotListed`.

## Reload

The table is compiled in. A change reaches sessions with an ants-mcpd
rebuild and an MCP reconnect.

## Build

Compiled into `test_claude`, which defines `SRC_CLAUDE_INTEGRATION_CPP_PATH`
and `ANTS_MCP_REGISTRY_SOURCE`.
