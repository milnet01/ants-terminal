# ANTS-4659 — an undeclared MCP argument is refused

A `tools/call` naming a top-level argument its verb does not declare is
refused before the verb runs. `compact` works on every verb. The decision and
the survey behind it are on roadmap item ANTS-4659.

## Surface

- `src/claudeintegration.cpp` — `handleMcpRequest` refuses; `transformReply`
  applies `compact`.
- `src/mcpprojection.{h,cpp}` — `mcp::ignoredArgs`, `mcp::isUniversalDispatchArg`.

## Invariants

Each is checked by the test in `test_mcp_undeclared_args_refused.cpp` whose
name opens with its number.

- **INV-1** — A call carrying a top-level argument the verb's schema does not
  declare replies `ok:false`, `code:"bad_args"`. `unknown_args` lists every
  such argument, sorted, and `error` names each. The reply carries no
  `ignored_args`.
- **INV-2** — The verb's handler does not run on a refused call. It runs on
  the same call without the undeclared argument.
- **INV-3** — `caller_cwd`, `encoding`, `fields` and `compact` are never
  refused, on any verb.
- **INV-4** — `compact:true` drops the reply's `false` and empty fields on
  every verb, including one outside the default-compaction table
  (`kDispatchProjection`). There, an absent `compact` or `compact:false`
  leaves the reply whole.
- **INV-5** — `tool_info {name}` shows `compact` among the arguments of a verb
  whose own schema does not write it.
- **INV-6** — `offload` and `etag_match` are refused on a verb that does not
  honour them, and accepted on one that does. The test uses `doc_lint`, which
  honours `offload` and not `etag_match`.
- **INV-7** — A call is not refused while the server knows no schema for its
  verb: a verb absent from the tools list, or any verb before the first
  `tools/list`. Claude Code lists on connect, and `ants-mcpd --call` lists
  first (`src/mcpdcall.cpp`).

## Tests

The tests drive `ClaudeIntegration::handleMcpLine` with a counting handler
registered under `doc_lint`, a verb outside `kDispatchProjection`.
