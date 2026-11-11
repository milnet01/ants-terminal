# ANTS-1432 — `token_usage` failed-call metric

**Status:** in flight (Bundle C pull 11, 2026-05-16).
**Kind:** perf (observability).
**Lanes:** `tokenusageengine`, `claudeintegration`, `remotecontrol`.
**Source:** vestige-cc-feedback-2026-05-16.
**Pairs with:** ANTS-1402 (the unified dispatch observation point we
hook into); ANTS-1429 (the silent-empty fix that motivated the
question of "what *did* the failed call cost?").

## Problem

`token_usage` reports per-tool **savings** on successful calls but
doesn't surface per-tool **waste** on failed calls. A caller can't
see that a tool is net-negative until the failed-call cost
dominates.

Direct trigger: Vestige CC's 2026-05-16 observation — *"MCP cost
tokens for the failed query and saved none."* The failed
`roadmap_query` against a non-Ants-format roadmap charged input
tokens for the request envelope and output tokens for the (now
informative, post-1429) error envelope, but `token_usage` showed
zero entries for `roadmap_query` because the failed branch
short-circuited the counter.

The same gap would have helped during the ANTS-1422 investigation:
we'd have seen `token_usage` itself accumulating waste-bytes while
the bypass wasn't yet in place. Failure-cost visibility is a
debugging tool, not just a measurement curiosity.

## Goal

Surface per-tool failed-call cost in the `token_usage` envelope, so
callers can compute net-token-impact = `est_tokens_saved` −
(`failed_bytes_in` + `failed_bytes_out`) / 4.

## Design

### Engine surface (`src/tokenusageengine.h`)

`ToolCounter` and `ToolReport` each gain three fields:

```cpp
qint64 failedCalls    = 0;
qint64 failedBytesIn  = 0;
qint64 failedBytesOut = 0;
```

`Snapshot` gains one summary field:

```cpp
qint64 totalFailedBytes = 0;  // sum of failedBytesIn + failedBytesOut
                              // across ALL tools, even those filtered
                              // out of calls[] by include_zero:false
```

`recordCall` gains a `bool success = true` parameter (default
preserves the existing test-helper signature on non-failing call
paths):

```cpp
void recordCall(const QString &toolName,
                qint64 bytesIn,
                qint64 bytesOut,
                qint64 wrapBytes = 0,
                qint64 durationUs = 0,
                bool   success    = true);
```

### Engine accumulator (`src/tokenusageengine.cpp`)

On failure (`success == false`) we increment **only** the failed-*
fields. `nCalls`, `bytesIn`, `bytesOut`, `wrapBytes`, and the
duration min/max/sum are **untouched**. Rationale: the existing
success-path metrics tell the "what did this tool cost when it
worked" story; mixing failed-call bytes into the same buckets would
muddy `est_tokens_saved` (which subtracts observed bytes from
modelled-baseline bytes — a failed call's bytes don't belong in that
subtraction).

On success, behaviour is byte-identical to today; the failed-*
fields stay untouched. The two branches are mutually exclusive.

```cpp
if (success) {
    // existing accumulation (nCalls, bytesIn, bytesOut, wrap, dur)
} else {
    c.failedCalls    += 1;
    c.failedBytesIn  += bytesIn;
    c.failedBytesOut += bytesOut;
}
```

`buildReport` surfaces the new fields onto `ToolReport` and sums
`totalFailedBytes = Σ(failedBytesIn + failedBytesOut)` across all
counters (even ones filtered out of `calls[]` by `include_zero`).

### Dispatcher (`src/claudeintegration.cpp::recordDispatch`)

Today `recordCall` only fires on `result == "ok"`. After this
ticket, `recordCall` fires on **every** dispatch with
`success = (result == QLatin1String("ok"))`. The mcp_trace ring
behaviour is unchanged.

```cpp
const bool succeeded = (result == QLatin1String("ok"));
m_tokenUsage.recordCall(toolName, argBytes, outBytes,
                        wrapBytes, durUs, succeeded);
```

### Envelope (`src/remotecontrol.cpp::cmdTokenUsage`)

Per-tool object adds three fields; envelope adds one summary field:

```jsonc
{
  "tool": "roadmap_query",
  "n_calls": 0,
  "bytes_in": 0,
  "bytes_out": 0,
  "failed_calls": 1,
  "failed_bytes_in": 47,
  "failed_bytes_out": 318,
  "est_tokens_saved": 0,
  ...
}
```

```jsonc
{
  "total_saved": 12480,
  "total_wrap_bytes": 9001,
  "total_failed_bytes": 365,
  ...
}
```

### `include_zero` semantics

Today `include_zero:false` drops entries with `estTokensSaved == 0`.
That filter is **kept as-is** — a tool with only failed calls has
`estTokensSaved == 0` and would be dropped, which is wrong (we want
to surface the waste). Updated filter:

```cpp
if (r.estTokensSaved > 0 || r.failedCalls > 0) {
    snap.calls.append(std::move(r));
}
```

A tool that has zero calls of either kind is still dropped. A tool
that has only successes still appears if its saved > 0. A tool that
has only failures now appears (with `n_calls:0`, `failed_calls:N`).

## Invariants

- **INV-1.** Failed dispatch (`result != "ok"`) increments
  `failedCalls` by 1 and `failedBytesIn` + `failedBytesOut` by the
  observed wire sizes; leaves `nCalls`, `bytesIn`, `bytesOut`,
  `wrapBytes`, `durationUs*` untouched.
- **INV-2.** Successful dispatch (`result == "ok"`) leaves
  `failedCalls`, `failedBytesIn`, `failedBytesOut` untouched.
- **INV-3.** `Snapshot::totalFailedBytes` equals
  `Σ over all counters (failedBytesIn + failedBytesOut)`, including
  counters filtered out of `calls[]` by `include_zero`.
- **INV-4.** `include_zero:false` retains tools with
  `failedCalls > 0` even when `estTokensSaved == 0`. A tool with
  both counters at zero is still dropped.
- **INV-5.** Envelope schema: per-tool object carries
  `failed_calls`, `failed_bytes_in`, `failed_bytes_out`; envelope
  carries `total_failed_bytes`.

## Tests

`tests/features/token_usage_failed_metric/`:

- **`test_token_usage_failed_metric.cpp`** — pure-function tests on
  `TokenUsageEngine::Tracker`:
  - INV-1: `recordCall(..., success=false)` only touches failed-*.
  - INV-2: `recordCall(..., success=true)` only touches success-*.
  - INV-3: `Snapshot.totalFailedBytes` aggregates across all tools.
  - INV-4: `include_zero:false` retains failed-only tools.

Sibling MCP-layer envelope assertions are added to the existing
`tests/features/mcp_token_usage_tool/` fixture in a follow-up if
needed; v1 fixture is engine-only since `cmdTokenUsage` mechanically
forwards `Snapshot` fields onto the envelope (single source of
truth).

## RAM budget

Three `qint64` fields per `ToolCounter`. With ~30 distinct MCP
tools, the static-overhead headroom is 30 × 24 B = **720 B**.
`Snapshot::totalFailedBytes` is one additional `qint64`.
Negligible against the existing engine footprint.

## Build cost

`tokenusageengine.{h,cpp}` are in `ants_core_lib` and rebuilt by
every test target that links the engine. Adding fields to two
structs and a branch in one method does not add new translation
units. Expected impact: < 2 s incremental rebuild for the engine
plus its consumers.

## Migration

None. The `recordCall` signature is backward-compatible because the
new `success` arg has a `true` default. Existing test call sites
that don't pass `success` continue to record successful calls. The
single live call site in `recordDispatch` is updated in this
ticket.
