# token_usage_engine — feature contract

Pure-function tests for `TokenUsageEngine::Tracker` (ANTS-1284). See
`docs/specs/ANTS-1284.md` for invariant rationale; this is the
test-side mirror.

## What this test guards

The in-process per-tool MCP dispatch counter:

- **INV-1 / Exactness** — `recordCall` increments `n_calls` by
  exactly 1 per call and sums byte counts.
- **INV-2 / Wire-byte units** — the engine receives whatever byte
  counts the caller supplies; no implicit conversion. UTF-8 wire
  bytes captured at the dispatch site (`QString::toUtf8().size()`)
  flow through unchanged.
- **INV-3 / Reset atomicity** — `reset()` clears the counter map
  AND advances `sinceUnixMs` in the same call.
- **INV-4 / Saved-floor** — `est_tokens_saved` is `max(0, ...)`;
  responses that exceed the baseline report 0 saved, not negative.
- **INV-5 / Baseline lookup** — three baselines ship in v1
  (`roadmap_query=594000`, `verify_changes=8192`,
  `plan_template=8192`); unknown tools return 0.
- **INV-6 / Schema round-trip** — handled in the MCP-layer test
  (`mcp_token_usage_tool/`), not here (engine doesn't serialise).
- **INV-9 / Pure-read `buildReport`** — repeated calls return
  byte-identical snapshots.
- **INV-10 / Self-counted** — `recordCall("token_usage", …)`
  increments the `token_usage` counter (baseline 0 means 0 saved).
- **Sort order** — `calls[]` is sorted by `est_tokens_saved`
  descending, tiebreak by tool name ascending.
- **`include_zero` filter** — when false, tools with 0 saved are
  dropped from `calls[]` but still contribute to `tools_called`
  and `total_saved`.

## Bundle

`test_audit` — engine-style pure-function test, same family as
`test_verify_changes_engine` and `test_plan_template_engine`.
