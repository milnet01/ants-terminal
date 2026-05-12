# mcp_tools_list_schema

Pins the MCP-spec compliance contract for the Ants `tools/list`
JSON-RPC response: every tool entry must carry an `inputSchema` field.

## Contract

### Invariants

| # | Statement |
|---|-----------|
| 1 | Every tool in `tools/list` emits an `inputSchema` field. Missing the field causes Claude Code's Zod validator to reject the **entire** response and register zero tools. |
| 2 | Zero-argument tools (`get_cwd`, `get_session_info`, `get_last_command`, `get_git_status`, `get_environment`, `tab_list`) declare `{"type":"object"}` as their `inputSchema`. The MCP spec mandates the field even when there are no parameters. |
| 3 | The `emptySchema` object (shared by all zero-arg tools) sets `type` to `"object"`. It must not be constructed without this field, because an empty JSON object `{}` is not a valid JSON Schema. |
| 4 | Tools added in future must also declare `inputSchema`. The source must contain an `emptySchema` declaration with `type = "object"` that serves as the canonical shared sentinel, making compliance the path of least resistance for new tools. |

## Rationale

On 2026-05-12, six zero-arg tools were emitted without `inputSchema`.
Claude Code's Zod-based client validates `tools/list` strictly and
rejects the **whole** response when any entry is missing it — the
connection reports "Connected" but registers zero tools. Developers
wasted hours relaunching Claude Code looking for a network issue.

The fix was `QJsonObject emptySchema; emptySchema["type"] = "object";`
plus `xxxTool["inputSchema"] = emptySchema;` on each zero-arg tool.
This test locks that fix so a future tool addition that omits the
field breaks CI before it reaches production.

Evidence: `/home/ants/.cache/claude-cli-nodejs/…/mcp-logs-ants/2026-05-12T11-37-32-003Z.jsonl`
line 4 — `tools/list failed: tools[1].inputSchema expected object,
received undefined` (same for indices 2/3/4/5/7).

## Scope

**In scope:**
- Source presence of `emptySchema["type"] = "object"` declaration.
- Per-tool `inputSchema` assignment for each of the six zero-arg tools
  currently registered in `claudeintegration.cpp`.
- Shared sentinel pattern (`emptySchema`) as the forward-compatibility hook.

**Out of scope:**
- Live MCP socket round-trip validation (covered by integration tests).
- Schema correctness for parameterised tools (`get_scrollback`,
  `roadmap_query`, `get_text`) — those already had schemas before this bug.
- Zod validator behaviour inside Claude Code itself.

## Regression history

- **Introduced:** all versions prior to the 2026-05-12 fix (commit on `main`
  after ANTS-1255).
- **Fixed:** commit that added `emptySchema` + six `inputSchema` assignments
  in `claudeintegration.cpp` around line 1197–1232.

## Acceptance

Exit 0 = all 4 invariants hold.

Wired as a source file in `ants_add_gui_bundle(test_claude …)` in
top-level `CMakeLists.txt`. No per-feature `CMakeLists.txt`.
