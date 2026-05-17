# ANTS-1454 — `caller_cwd_required` refusals count as failed calls in `token_usage`

**Status:** in flight (Bundle E follow-up, 2026-05-17).
**Kind:** fix.
**Lanes:** `claudeintegration` (`processTools` dispatch site),
`mcp_caller_cwd_contracts` feature test.
**Source:** cold-eyes-review-2026-05-17 (during ANTS-1356); roadmap
entry under `ROADMAP.md § Ants-MCP follow-ups from ANTS-1356`.

## Problem

The ANTS-1404 `caller_cwd_required` refusal branch at
`src/claudeintegration.cpp:3570-3594` sets `toolHandled = true`
and falls into the shared `if (toolHandled) { … recordDispatch(…,
dispatchResult); }` path. `dispatchResult` defaults to
`"ok"` at the top of the dispatch block, and the refusal branch
did not override it. `recordDispatch` derives:

```cpp
const bool succeeded = (result == QLatin1String("ok"));
m_tokenUsage.recordCall(toolName, argBytes, outBytes,
                        wrapBytes, durUs, succeeded);
```

So every refused call recorded with `succeeded = true`, dropping
into `token_usage`'s successful-call accumulator. The cost
dashboard could not tell apart a successful call from a
misconfigured caller burning bytes on refusal envelopes —
exactly the visibility the ANTS-1432 split was supposed to
deliver.

The sibling refusal at line 3625 (rate-limit, ANTS-1356)
already overrode `dispatchResult = "rate_limited"`. ANTS-1356's
spec called out the latent ANTS-1404 case as a teed-up
follow-up; this fix retires it.

## Fix

One-line change inside the ANTS-1404 refusal branch: after
setting the envelope and `toolHandled = true`, append

```cpp
dispatchResult = QStringLiteral("caller_cwd_required");
```

The comment above the `QString dispatchResult = "ok";`
initialisation is updated to drop the now-stale "ANTS-1404
keeps ok for now" caveat and point at this spec. The
sub-comment next to the `recordDispatch` call is amended to
list the three known result strings (`"ok"`,
`"caller_cwd_required"`, `"rate_limited"`).

No schema change, no behavioural change to the refusal envelope
seen by the caller — only the internal accounting that drives
`token_usage`'s `failed_call_*` byte counters and the
`mcp_trace` ring's `result` field.

## Invariants

- **DISP-5.** The ANTS-1404 refusal branch sets
  `dispatchResult = QStringLiteral("caller_cwd_required")`
  before falling into the shared `recordDispatch` path.
  Source-grep on the dispatch region: literal must appear
  inside the branch closing `}\n`.

(DISP-1 through DISP-4 from the existing `mcp_caller_cwd_contracts`
spec.md are unaffected — the refusal envelope shape, the
`!toolHandled` cache gate, and the provider-dispatch guard all
keep their pre-1454 behaviour.)

## Test

Extend `tests/features/mcp_caller_cwd_contracts/test_mcp_caller_cwd_contracts.cpp`
with `TEST(McpCallerCwdContracts, RefusalSetsDispatchResult)`.
Pattern matches the file's existing source-scrape style — locate
the refusal envelope literal, walk forward to the branch
closing brace, assert the `dispatchResult = …
"caller_cwd_required"` literal appears inside.

## Out of scope

- `token_usage`'s envelope shape (still emits the same
  `successful_calls` / `failed_calls` split — ANTS-1432).
- The other refusal branches (`rate_limited` was already
  correct; `tool_not_found` already routes via the failure
  path at `claudeintegration.cpp:3777-3780`).
- Re-classifying the third dispatch result string vs. the
  rate-limit one. Both share the same failed-call bucket; the
  `result` field in `mcp_trace` is what distinguishes them at
  trace-grain.
