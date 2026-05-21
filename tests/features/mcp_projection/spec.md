# ANTS-1720 — MCP response projection (`fields=` parameter)

## Problem

High-volume MCP read tools return their full payload on every call even
when the caller needs one top-level field. `roadmap_query` alone can run
8–80 KB. A caller polling for `git_state`'s `branch`, or scraping
`roadmap_query`'s `bullets`, pays for the entire envelope each round-trip.

## Solution

A new optional `fields: ["f1","f2"]` array parameter on seven read tools
returns **only the named top-level fields**. The response *schema* is
unchanged — absent fields are simply omitted. Callers that omit `fields`
get the full payload (fully backwards-compatible).

Tools in scope (all seven are also etag-supported, ANTS-1499):

`roadmap_query`, `project_layout`, `file_outline`, `get_environment`,
`tab_list`, `subsystem`, `git_state`.

## Where it lives

Pure projection logic is `mcp::projectFields` + the allowlist
`mcp::isFieldProjectionTool` in `src/mcpprojection.{h,cpp}` (Qt6::Core
only — so the dispatch layer and this test share one implementation,
mirroring `focusedtest` / `modelrecommender`).

The dispatch site (`ClaudeIntegration`, `tools/call` branch) calls
`mcp::projectFields` **after** the ETag short-circuit and **before**
`wrapMcpData`. This ordering is the load-bearing invariant: the etag is
computed on the *unfiltered* canonical body, so a narrowed call still
short-circuits when state is unchanged.

## Invariants

- **INV-1 — full payload on absent/empty `fields`.** `projectFields(body,
  emptyArray)` returns `body` byte-for-byte. The dispatch only calls
  `projectFields` when `fields` is a present, non-empty array.
- **INV-2 — single-field subset correct.** `projectFields` of a body with
  `fields=["bullets"]` returns an object containing exactly the `bullets`
  key, with its value copied verbatim.
- **INV-3 — multi-field subset preserves only named keys, verbatim.**
  `fields=["branch","files"]` yields `{branch,files}` and nothing else.
- **INV-4 — unknown field name yields an empty object, never an error.**
  `fields=["nonexistent"]` returns `{}`. A mix of known + unknown returns
  only the known keys.
- **INV-5 — non-string / empty field entries are ignored**, not faulted.
- **INV-6 — non-object response bodies pass through unchanged** (the
  projection only applies to JSON-object envelopes).
- **INV-7 — etag is NOT auto-preserved.** To keep the etag for a
  follow-up 304 call, the caller lists `"etag"` in `fields`. Because the
  dispatch computes the etag on the unfiltered body *before* projecting,
  a `fields=["bullets","etag"]` response carries the same etag a full
  call would (this is what "etag computed on canonical body, not filtered
  body" means).
- **INV-8 — allowlist is the seven in-scope tools only.**
  `isFieldProjectionTool` returns true for exactly those seven and false
  otherwise (e.g. `get_scrollback`, `session_brief`).
- **INV-9 — dispatch ordering.** In `claudeintegration.cpp` the
  `projectFields` call appears after `applyEtagPattern` and before the
  `wrapMcpData` call, and is guarded so the etag short-circuit
  (`{ok,unchanged,etag}`) is never narrowed.
- **INV-10 — schema declares `fields`.** Each of the seven tools'
  `inputSchema.properties` carries a `fields` array-of-string property.
