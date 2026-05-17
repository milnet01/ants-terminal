# mcp_selection_hint — ANTS-1453

See `docs/specs/ANTS-1453.md`.

## Test scope

Source-scrape regression locks the per-tool `selection_hint`
descriptor field and the `tool_info` pass-through.

## Invariants checked

- **HINT-1.** Every entry appended to the `tools` array inside the
  `tools/list` handler in `claudeintegration.cpp` sets
  `[X]Tool["selection_hint"]` (or `t["selection_hint"]`) to a
  `QStringLiteral` literal. The source-grep walks every
  `["name"] = "tool_name"` declaration and asserts a matching
  `["selection_hint"]` assignment exists before the next descriptor.
- **HINT-2.** `tool_info`'s success-envelope branch passes
  `selection_hint` through from `match.value(...)` — no defaulting,
  no falling back to description copy.
- **HINT-3.** No `selection_hint` string exceeds 240 chars (rough
  budget so a 42-tool dump stays under ~10 KB).
