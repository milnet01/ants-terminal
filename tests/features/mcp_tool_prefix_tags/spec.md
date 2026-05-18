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
- **INV-3.** No registered tool name (the 40+ tools wired via
  `registerToolProvider` in `src/mainwindow.cpp`) falls into the
  `"other"` bucket. Source-scrape walks both files; every name in
  `mainwindow.cpp`'s `registerToolProvider(...)` call list must
  appear in `kindForName`'s branch list (idempotency / coverage
  check).

## Scope

Two-line label rename + one feature test + spec. No descriptor
shape change; downstream consumers (`tools/list`, `mcp_trace`'s
`kind:` field, `token_usage`'s per-tool buckets) all read the
bucket via the same mapping.
