# ANTS-1567 — Every MCP tool description carries a non-`[other]` prefix tag

## Problem

Music_Production 2026-05-18 + MAME Curator 2026-05-18 noticed the 40+
Ants MCP tool surface arrives as a flat alphabetical block in the
deferred-tools list. ANTS-1518 already prepends a `[<kind>] ` prefix
to every tool's description via `kindForName` in
`src/claudeintegration.cpp::processTools`. But:

1. **Coverage drift risk.** If a new tool is registered without a
   matching `kindForName` branch, the prefix falls back to
   `[other]` — silent loss of grep-friendliness.
2. **Label semantics.** The original 2026-05-13 mapping called
   `session_memory` `[memory]` (ambiguous with RAM / terminal
   buffer) and put `caller_cwd_info` under `[terminal]` (it's a
   diagnostic verb, not a pty-state read). The cross-session
   reports asked for sharper labels.

## Fix

1. Rename `memory` → `mcp-state` in `kindForName` (server-side
   per-cwd KV store, not "memory" generically).
2. Move `caller_cwd_info` from `terminal` to `meta` (diagnostic
   verb, not a pty-state read).
3. Add a feature test asserting every registered MCP tool name
   resolves to a non-`other` prefix.

## Invariants

- **INV-1.** `kindForName("session_memory")` returns `mcp-state`.
- **INV-2.** `kindForName("caller_cwd_info")` returns `meta`.
- **INV-3.** No registered tool name (the tools wired via
  `registerToolProvider` in `src/mainwindow.cpp`) falls into the
  `"other"` bucket. Source-scrape walks both files; every name in
  `mainwindow.cpp`'s `registerToolProvider(...)` call list must
  appear in `kindForName`'s branch list (idempotency / coverage
  check).
- **INV-4.** No tool's authored short `description` begins with `[`.
  The tools/list prefix loop prepends `[<kind>] ` only when the
  description does not already start with `[`. So an authored bracket
  suppresses the tag: the tool shows its author's text, not the tag
  `kindForName` assigns. INV-3 cannot see this, because the bucket
  mapping is still correct. The set is every `["name"] = "<tool>"`
  assignment in `src/claudeintegration.cpp` except `serverInfo`'s. That
  is wider than INV-3's set: it includes `tool_info` and
  `get_session_info`, which are built inline rather than registered
  through `registerToolProvider`.

## Scope

Two-line label rename + one feature test + spec. ANTS-3645 part (b)
added INV-4. No descriptor shape change; downstream consumers
(`tools/list`, `mcp_trace`'s `kind:` field, `token_usage`'s per-tool
buckets) all read the bucket via the same mapping.

## Regression history

ANTS-3645 part (b) (2026-09-10). `mcp_tool_detail_field` INV-6 already
made this claim, but only for the tools its `kInScope` names. INV-4
widens it to every tool. `build_target_for` and `test_audit_recheck`
were live examples: their descriptions began `[build]` and
`[test-audit]`.
