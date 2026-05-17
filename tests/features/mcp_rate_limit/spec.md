# Feature spec: MCP per-tool rate-limit / quota (ANTS-1356)

Per-(toolName, callerCwd) sliding-window rate-limit on the MCP
dispatcher. Three tiers: Cheap (60 calls/60 s), Expensive (10
calls/60 s), ControlPlane (uncapped). Refuses with
`{ok:false, code:"rate_limited", retry_after_ms}` when the bucket
is full. See `docs/specs/ANTS-1356.md` for the full design.

## Invariants

- **INV-1 / under-cap accepts.** A call below the cap returns 0
  (accepted) and appends the timestamp to the bucket.
- **INV-2 / at-cap refuses with positive retry_after_ms.** Exactly
  `capPerWindow` Expensive calls within the 60 s window cause the
  next call to refuse with `retry_after_ms = (ts[0] + 60'000) - now`.
- **INV-3 / post-window allowed again.** A call at
  `nowMs ≥ oldest_ts + windowMs` is accepted; the prune-front step
  drops the expired entry.
- **INV-4 / ControlPlane never refuses.** A `ControlPlane` tool
  always returns 0 and does not grow `m_rateLimitBuckets`.
- **INV-5 / cache hits consume.** Source-grep proves
  `rateLimitCheck` runs BEFORE `tryGetIdempotentReadCache` so a
  cacheable tool at the cap refuses even when the underlying call
  would be cache-served.
- **INV-6 / empty buckets auto-pruned.** After all entries fall out
  of the window, the next rateLimitCheck removes the bucket entry;
  `rateLimitBucketCountForTest()` drops back to its prior value.
- **INV-7 / map cap enforced.** `m_rateLimitBuckets` never exceeds
  `kRateLimitMapCap = 256` entries; insertion at the cap evicts
  the bucket with the oldest most-recent timestamp.
- **INV-8 / per-(tool, cwd) isolation.** `("audit_run", "/proj/A")`
  and `("audit_run", "/proj/B")` get independent buckets.
- **INV-9 / caller-cwd-less buckets share.** Two `roadmap_query`
  calls with empty `caller_cwd` count toward the same
  `(toolName, "")` bucket.
- **INV-10 / refusal routed through recordDispatch.** Source-grep
  proves the refusal branch sets `dispatchResult = "rate_limited"`
  and that `recordDispatch` is called with `dispatchResult`
  (not the literal `"ok"`).
- **INV-11 / refusal envelope shape.** Source-grep verifies the
  envelope has `ok:false`, `code:"rate_limited"`, `retry_after_ms`,
  and an `error` string.
- **INV-12 / order vs caller_cwd_required.** Source-grep verifies
  the `caller_cwd_required` check in `processTools` runs before
  the rate-limit branch.
- **INV-13 / tier classification source-grep.** The ControlPlane
  allowlist contains exactly the five expected tools; the
  Expensive list contains the documented heavy verbs.
- **INV-14 / test-only cap override.** `setRateLimitCapsForTest`
  + `resetRateLimitCapsForTest` round-trip cleanly.
- **INV-15 / probe accessors.** `rateLimitBucketCountForTest()`
  and `rateLimitBucketDepthForTest(tool, cwd)` return correct
  values on empty / one-entry / post-prune-empty states.
- **INV-16 / monotonic clock declared.** Source-grep verifies a
  `static QElapsedTimer s_rateLimitClock` lives at TU scope in
  `claudeintegration.cpp` and is started in the constructor.
