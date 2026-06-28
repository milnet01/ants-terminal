# mcp_ignored_args — unknown-arg advisory (ANTS-2175)

## Problem

The MCP `tools/call` dispatcher silently drops args a verb does not
declare. A `query="token"` passed to `roadmap_query` (whose filters are
`status` / `section` / `id` / `mode` — there is no `query`) was ignored,
and the verb returned the full unfiltered roadmap — which *looked* like a
working search. The typo cost several follow-up calls to notice the filter
was a no-op.

## Surface

- `src/mcpprojection.{h,cpp}` — `mcp::ignoredArgs(args, known)`: the pure
  diff (Qt6::Core only, in `ants_core_lib`, shared by the dispatch layer and
  this test, mirroring `projectFields` / `offloadRequested`).
- `src/claudeintegration.{h,cpp}` — `m_toolParamKeys` cache (rebuilt at the
  end of the `tools/list` handler from each tool's `inputSchema.properties`)
  and the dispatch-site injection that attaches `ignored_args` to the
  success envelope.

## Invariants

- **INV-1** — `ignoredArgs` returns, sorted ascending, exactly the keys in
  `args` that are neither in `known` nor a universal dispatch-layer arg.
- **INV-2** — the universal dispatch-layer args (`caller_cwd`, `etag_match`,
  `fields`, `compact`, `offload`) are NEVER reported, even when a verb's
  `known` set does not redeclare them — the dispatcher accepts them for
  every verb.
- **INV-3** — when every arg is recognised (in `known` or universal), the
  result is empty.
- **INV-4** — an empty `known` set reports every non-universal arg (a verb
  that declares no properties still accepts the universal args).
- **INV-5** — the `tools/list` handler populates `m_toolParamKeys` from the
  assembled tools array's `inputSchema.properties`, rebuilt in lockstep with
  `m_lastToolsList`.
- **INV-6** — the dispatch site calls `mcp::ignoredArgs`, attaches an
  `ignored_args` field, and is gated so it runs only on a freshly-dispatched
  call (`!cachedHit`) and never annotates a refusal (`ok:false`).

## Tests

`test_mcp_ignored_args.cpp` — behavioural coverage of `mcp::ignoredArgs`
(INV-1..INV-4) plus source-scrapes of `claudeintegration.cpp` for the cache
population and dispatch injection (INV-5, INV-6).
