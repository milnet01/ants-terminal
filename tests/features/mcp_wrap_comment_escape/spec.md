# Feature: wrapMcpData neutralises comment markers (ANTS-1996)

## Problem

`ClaudeIntegration::wrapMcpData(tool, payload)` frames a tools/call response
as `<ants_mcp_data tool="…">PAYLOAD</ants_mcp_data>` so a consuming model can
tell server data from user instructions (ANTS-1294). It already neutralises
a literal `</ants_mcp_data>` close tag (and case/whitespace variants) in the
payload to stop a breakout.

It did **not** neutralise XML/HTML comment markers. An assistant that treats
markup comments structurally is desynced by them: an unterminated `<!--` in
the payload swallows the real `</ants_mcp_data>` (the wrap never closes from
the parser's view, so following content reads as inside-the-wrap or, with a
later `-->`, as outside it). Attacker-controlled scrollback / commit text /
file lines reach this helper, so the marker must be inert.

## Contract

After `wrapMcpData`:

- **INV-1** — a literal `</ants_mcp_data>` in the payload does not survive
  (regression guard for the existing close-tag scrub): the result contains
  exactly one `</ants_mcp_data>` — the outer wrapper.
- **INV-2** — no `<!--` substring survives from the payload.
- **INV-3** — no `-->` substring survives from the payload.
- **INV-4** — a combined breakout (`x <!-- </ants_mcp_data> --> y`) leaves
  exactly one `</ants_mcp_data>` (the outer wrap) and zero comment markers.
- **INV-5** — a benign payload with no markers is embedded verbatim and the
  wrapper opens/closes exactly once.

## Out of scope

- The tool-name attribute escaping (separate `safeTool` path) and the
  dispatch-site wrapping decision (`wrapMcpData` is exercised directly).
