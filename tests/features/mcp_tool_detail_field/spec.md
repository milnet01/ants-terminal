# Feature: MCP tool `description`/`detail` split (ANTS-2079)

Trim the load-bearing per-op prose out of the seven largest MCP tool
`description` strings (the bytes every session pays for in `tools/list`)
into a sibling `detail` literal that is **stripped from the wire payload**
and served on demand by `tool_info {name}`. Full design:
[`docs/specs/ANTS-2079.md`](../../../docs/specs/ANTS-2079.md).

In-scope tools (wire `description` ≥ ~1800 B pre-trim): `roadmap_query`,
`model_switch_stats`, `roadmap_log`, `test_audit_partition`,
`changelog_log`, `workspace_search`, `verify_changes`.

This is a **source-scrape** test (same harness as `mcp_tool_info_verb`):
it reads `src/claudeintegration.cpp` and asserts the structural facts
that implement each invariant, plus a reconstructed wire-byte budget for
INV-5. There is no live-dispatcher seam in this bundle, so the runtime
behaviours are validated through the source constructs that produce them.

## Invariants

- **INV-1** — The `tools/list` handler strips `detail` from the wire
  array: a `t.remove(...detail...)` runs over `tools` after the snapshot.
- **INV-2** — `tool_info` serves `detail` **conditionally**:
  `if (match.contains(...detail...)) env["detail"] = ...` — present only
  for tools that authored one, so the ~66 others omit the key.
- **INV-3** — For each in-scope tool, every anchor token (refusal codes,
  op/selector names, load-bearing envelope fields) in
  `anchors/<tool>.txt` still appears in the tool's source block (the
  union of its `description` + `detail` literals). Nothing silently
  dropped. Anchors captured mechanically from the pre-trim source.
- **INV-4** — The wire strip appends the runtime pointer
  `Full per-op detail via tool_info {name:"%1"}` to each trimmed
  description.
- **INV-5** — Each in-scope tool's reconstructed wire `description`
  (runtime `[<kind>]` prefix + short literal + Etag-tip memo on
  etag-supported tools + the per-op pointer) is ≤ 800 B.
- **INV-6** — Each in-scope tool's short `description` literal does NOT
  itself begin with `[`, so the runtime `[<kind>]`-prefix loop fires and
  `kindForName` keeps bucketing it (guarded end-to-end by
  `mcp_tool_prefix_tags` INV-3, which must stay green).
- **INV-7** — Order is load-bearing: the snapshot (`m_lastToolsList =
  tools`) precedes the strip, and the strip precedes the wire send — so
  the snapshot retains the clean short `description` + the `detail`
  sibling, and the pointer never leaks into the snapshot `tool_info`
  reads.

INV-8 (no regression in the pre-existing source-scrape window tests) is
covered by those tests staying green under `ctest`, not by an assertion
here.
