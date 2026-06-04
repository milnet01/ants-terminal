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

## ANTS-1985 — catalog mode (INV-9..INV-14)

See `docs/specs/ANTS-1985.md`. `tool_info {catalog:true}` returns every
registered verb grouped by category with its `selection_hint`, in one
call. INV-9..INV-13 are source-scrape tests here; INV-14 lives in
`mcp_orientation_install` (the prelude template owner).

- **INV-9.** The `tool_info` descriptor declares a boolean `catalog`
  property, keeps `additionalProperties:false`, and no longer lists
  `name` in `required`.
- **INV-10.** The catalog branch emits the grouped envelope keys
  (`catalog`, `tool_count`, `category_count`) via a `catalogMode` flag.
- **INV-11.** Catalog mode with an empty `m_lastToolsList` emits
  `tools_not_ready`; the catalog branch precedes the `missing_name`
  guard.
- **INV-12.** Category is derived from the `[<kind>]` description prefix
  with an `other` fallback (absent / malformed / empty `[]`); grouping
  is via a sorted `QMap` so categories and names emit ascending.
- **INV-13.** Legacy branches (`missing_name`, `unknown_tool`,
  single-tool slice) are intact and catalog mode is gated on the
  explicit `catalog` arg, never on empty `name`.
- **INV-14.** (in `mcp_orientation_install`) The prelude template's
  "Full catalog:" line is repointed at `tool_info {catalog:true}` and
  the rendered heredoc stays ≤ 1200 bytes.
