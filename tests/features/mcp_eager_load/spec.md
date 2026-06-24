# Feature: eager-load high-frequency MCP verbs (ANTS-2158)

Test contract for the deferred-schema-tax mitigation. Claude Code defers
MCP tool schemas (tool-search): a deferred verb needs a ToolSearch
round-trip before its first call, while Bash grep / Read / Edit are
always loaded — a friction gradient that nudges a long session back to
raw grep (Vestige Obs #18, confirmed cross-session).

The fix is server-side: Claude Code (v2.1.121+) honours
`"anthropic/alwaysLoad": true` in a tool's `_meta` object in the
`tools/list` response, exempting that tool from deferral. Older clients
ignore the field (graceful). Verified against
code.claude.com/docs/en/mcp.md (2026-06-24).

A small, curated set is marked — each always-loaded tool costs context,
so only the verbs that most directly replace always-loaded built-ins:
`workspace_search`, `find_definition`, `file_outline`, `read_region`
(grep / Read substitutes) and `roadmap_log` / `changelog_log`
(ROADMAP/CHANGELOG Edit substitutes).

## Invariant

- The `tools/list` builder (`src/claudeintegration.cpp`) marks each verb
  in the curated `kEagerVerbs` set with `"anthropic/alwaysLoad": true`
  under its `_meta`. *Test:* source-grep for the field, the set name, and
  the six verb names.

## Pre-fix check

Against pre-fix code the `anthropic/alwaysLoad` field and `kEagerVerbs`
set are absent → the assertions fail. Verified before wiring.

Label: `features;fast`.

## Note on what is NOT server-controllable

A server-wide "eager-load this whole server" directive does not exist;
the only server lever is the per-tool `_meta` hint (and the client-side
`alwaysLoad` per-server key in `.mcp.json`, which a heavy user can set
themselves). The deferral mechanism itself is a Claude Code architectural
choice.
