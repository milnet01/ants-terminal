# mcp_tool_info_verb — ANTS-1399

See `docs/specs/ANTS-1399-tool-info-name-mcp-verb.md`.

## Test scope

Source-scrape regression locks the `tool_info` MCP verb's
schema, classification, dispatch path, and the error-envelope
codes the handler emits.

## Invariants checked

- **INV-1.** `tool_info` registered as an MCP tool in
  `claudeintegration.cpp`'s `tools/list` block with `name`
  required. Its schema's strictness is `mcp_undeclared_args_refused` INV-8.
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

See `docs/specs/ANTS-1985-tool-info-catalog-true-one.md`. `tool_info {catalog:true}` returns every
registered verb grouped by category with its `selection_hint`, in one
call. INV-9..INV-13 are source-scrape tests here; INV-14 lives in
`mcp_orientation_install` (the prelude template owner).

- **INV-9.** The `tool_info` descriptor declares a boolean `catalog`
  property and no longer lists
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

## ANTS-6829 — one op's slice

`tool_info {name, op}` returns `mcp::toolInfoForOp` (`src/mcptoolinfo.h`)
for the matched descriptor. Behavioural cases on a made-up two-op verb:

- **`Ants6829SliceKeepsOneOpsArgumentsAndProse`** — `op:"add"` keeps the
  arguments naming `"add"` or no op, drops one naming only `"drop"` (a
  nested description counts) and the `op` argument, lists the dropped
  names in `other_arguments`, filters `required` to the kept names, and
  builds `op_detail` from `add`'s runs in `detail` then in the `op`
  argument.
- **`Ants6829UnknownOpAndOplessVerbRefuse`** — an op not in the enum
  refuses `unknown_op` with `ops[]`; a verb with no `op` argument refuses
  `bad_args`.

The wire path is `McpdCall.Ants6829ToolInfoServesOneOp`: through
`ants-mcpd --call`, `roadmap_log` with `op:"append"` keeps `before_id`,
drops `amendments`, and is under half the whole descriptor's size.

**Would break this:** the handler ignoring `op`, or `op` left out of the
`tool_info` schema, which refuses it as an unknown argument.
