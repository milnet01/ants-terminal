# mcp_output_sanitisation — feature-conformance contract

Locks ANTS-1294: MCP `tools/call` responses frame user-supplied
content as data, not instructions, by wrapping the payload in
`<ants_mcp_data tool="…">…</ants_mcp_data>` at the single
chokepoint in `src/claudeintegration.cpp:processTools`.

Spec source: `docs/specs/ANTS-1294.md`.

## Invariants exercised

- **INV-1 wrap shape.** `ClaudeIntegration::wrapMcpData("get_text",
  "hello")` returns
  `<ants_mcp_data tool="get_text">hello</ants_mcp_data>`. The tool
  name is the literal argument; no quoting needed because all 30
  registered names match `^[a-z][a-z0-9_]+$`.

- **INV-3 close-tag neutralisation.** Embedded `</ants_mcp_data>`
  substrings in the payload are replaced with the self-closing
  sentinel `<ants_mcp_data_escaped/>` before wrapping. After the
  wrap, the result contains exactly one `</ants_mcp_data>` (the
  closing tag) and one sentinel per replacement.

- **INV-5 case-sensitivity.** Only the exact case-sensitive
  `</ants_mcp_data>` is replaced. Variants
  (`</ANTS_MCP_DATA>`, `< /ants_mcp_data >`, etc.) round-trip
  unchanged inside the wrap.

- **INV-4 binary cleanliness.** A payload that does not contain
  the close-tag substring round-trips byte-for-byte inside the
  wrap. UTF-8 multi-byte sequences, control characters, NULs, and
  high-byte values are preserved.

- **Dispatch-site wiring (REG-*).** Source-grep on
  `claudeintegration.cpp` and `claudeintegration.h` pins:
  - `wrapMcpData` is declared `static` on `ClaudeIntegration` in
    the header.
  - The dispatch block at `// ANTS-1284 — record dispatch` is
    preceded by a `wrapMcpData(toolName, responseText)` call gated
    by an `isControlPlane` check.
  - The control-plane exempt set is exactly
    `{get_session_info, token_usage}`.
  - `CLAUDE.md` "Conventions" mentions `ants_mcp_data` so the
    convention is discoverable.

## Test layout

`test_mcp_output_sanitisation.cpp` is one bundle source file in
the `test_claude` target (it already links `ants_claude_lib` and
defines `SRC_CLAUDE_INTEGRATION_CPP_PATH` / `_H_PATH` /
`ANTS_CLAUDE_MD_PATH`). Mixes:

- Engine-level GoogleTest cases that call
  `ClaudeIntegration::wrapMcpData` directly (the function is
  `static` and reachable from the bundle's link surface).
- Wiring-level cases that slurp the cited source files and assert
  expected substring presence/count.

## Pre-fix regression check

If `wrapMcpData` is not defined, the engine tests fail to link
(test_claude bundle build error). If the dispatch site is reverted
to the pre-1294 `result["content"] = makeTextContent(responseText);`
single-liner, the REG cases at the dispatch site fail. Both
failures are loud.
