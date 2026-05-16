# token_usage_failed_metric — feature contract

Pure-function tests for the ANTS-1432 failed-call metric extension to
`TokenUsageEngine::Tracker`. See `docs/specs/ANTS-1432.md` for the
full design + rationale; this is the test-side mirror.

## What this test guards

The failed-call accumulator on `TokenUsageEngine::Tracker`:

- **INV-1 / Failed branch isolates** — `recordCall(..., success=false)`
  increments `failedCalls`, `failedBytesIn`, and `failedBytesOut`;
  leaves `nCalls`, `bytesIn`, `bytesOut`, `wrapBytes`, and
  `durationUs*` untouched.
- **INV-2 / Success branch isolates** — `recordCall(..., success=true)`
  leaves `failedCalls`, `failedBytesIn`, and `failedBytesOut`
  untouched.
- **INV-3 / Envelope summary aggregates** —
  `Snapshot::totalFailedBytes` equals
  `Σ(failedBytesIn + failedBytesOut)` across ALL counters,
  including counters filtered out of `calls[]` by `include_zero:false`.
- **INV-4 / Filter retains failure-only tools** —
  `include_zero:false` retains tools with `failedCalls > 0` even
  when `estTokensSaved == 0`. A tool with both counters at zero
  is still dropped.

## Bundle

`test_audit` — engine-style pure-function test, same family as
`test_token_usage_engine`.
