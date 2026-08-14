# mcp_raw_read — opt-in verbatim framing for content reads (ANTS-2218)

## Problem

The MCP dispatch wraps every non-control-plane tool result in the
`<ants_mcp_data tool="…">…</ants_mcp_data>` frame and neutralises any literal
`</ants_mcp_data>` (plus open-tag / `<!--` / `-->` variants) inside the payload
so hostile content can't forge the frame-close (ANTS-1294 / 1670 / 1996). That
scrub is intentionally lossy and irreversible — any escaping a good agent could
invert, a hostile normalising tokeniser could invert too. The cost: when an
agent reads SOURCE that legitimately contains those tokens (this MCP source, a
spec, an HTML/markdown file with comments) and builds an `Edit`/`apply_edits`
from the read output, it edits from doctored bytes and corrupts the file
(ANTS-2218).

## Surface

- `ClaudeIntegration::wrapMcpDataRaw(toolName, payload)` — verbatim framing in
  an unforgeable nonce frame.
- `mcp::isRawEligible(toolName)` — the read verbs that honour `raw:true`.
- Dispatch seam in `src/claudeintegration.cpp` — reads `raw`, suppresses the
  ANTS-2094 offload, branches the wrap.
- `raw` boolean schema prop on `read_region` / `read_regions` /
  `workspace_search`.

## Invariants

- **INV-1 verbatim** — a payload containing `</ants_mcp_data>`, `<!--` and
  `-->` is embedded byte-for-byte by `wrapMcpDataRaw` (no substitution),
  whereas `wrapMcpData` neutralises all three (the contrast that motivates the
  feature).
- **INV-2 well-formed frame** — the output is
  `<ants_mcp_data_raw__<nonce> tool="X"><payload></ants_mcp_data_raw__<nonce>>`;
  the open-tag and close-tag nonce match.
- **INV-3 unforgeable** — the nonce is a content-hash verified absent from the
  payload, so the real close tag occurs exactly once in the output even when
  the payload embeds a literal `</ants_mcp_data_raw__deadbeef>` with a guessed
  nonce.
- **INV-4 tool-name hardening** — `"` / `<` / `>` / `&` in `toolName` are
  entity-escaped in the `tool="…"` attribute (mirrors `wrapMcpData`).
- **INV-5 eligibility** — `isRawEligible` is true for `read_region` /
  `read_regions` / `workspace_search` / `file_outline`, false for `get_text` /
  `apply_edits` / `roadmap_query` / `get_session_info`.
  `file_outline` joined the set in ANTS-4365: it returns `header_doc` (and
  `signature`) as file bytes, so it is a content read by the same test the
  other three pass, and without the escape a Markdown file whose header is an
  HTML comment could not report its own first line truthfully.
- **INV-6 wiring** — the dispatch suppresses offload under `raw` and routes to
  `wrapMcpDataRaw`; `makeRawProp` declares the schema prop. (Source-scrape — the
  dispatch glue isn't unit-testable standalone.)
