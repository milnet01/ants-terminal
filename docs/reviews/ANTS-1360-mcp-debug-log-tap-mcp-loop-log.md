# ANTS-1360 — MCP debug-log tap (`mcp_trace` tool) — review loop log

Record for [`../specs/ANTS-1360-mcp-debug-log-tap-mcp.md`](../specs/ANTS-1360-mcp-debug-log-tap-mcp.md) § 8. Cold-eyes review log.

## Cold-eyes loop log

### Pass 1 — 2026-05-15 — HIGH:1 / MEDIUM:5 / LOW:2

- **HIGH-1 — `next_id` cursor skip bug.** INV-3 was framed as
  `id > since` while `next_id` was `max_id + 1`. A polling client
  using `since = previous next_id` would have called with
  `since = highest_seen + 1`; the filter would then return
  `id > highest_seen + 1`, skipping the *next* record. **Fixed**
  in pass: INV-3 changed to inclusive `id >= since`; `next_id`
  remains `max_returned + 1` (correct under the new filter);
  empty-response semantics added (`next_id == since` input
  echoed) so a polling client doesn't get an off-by-one slide.

- **MEDIUM-1 — Registration location underspecified.** Spec § 4
  said "register in the setup path" but didn't pin
  MainWindow vs. ClaudeIntegration. **Fixed** in pass: added
  § 2.1.2 — registration lives in MainWindow alongside the other
  25 provider registrations; lambda captures
  `[ci = m_claudeIntegration]` and calls public
  `ClaudeIntegration::queryMcpTrace(since, limit)`.

- **MEDIUM-2 — Timer placement underspecified.** § 2.2 mentioned
  QElapsedTimer but didn't say where the timer is started or
  stopped. **Fixed** in pass: § 2.1.1 specifies start before
  cache lookup (so cache-hit dispatches record their true
  µs-range latency), stop right before the record call.

- **MEDIUM-3 — Failure-path field details.** INV-7 said unknown-
  tool failure produces a record but didn't say what `resp_bytes`
  and `duration_us` look like. **Fixed** in pass: new INV-12 —
  failure records have `resp_bytes:0` and a non-negative
  `duration_us`. New test 15 (FailurePathRespBytesZero) locks it.

- **MEDIUM-4 — `arg_keys` recursion depth unbound.** § 2.4 said
  nested values report as `array<N>` / `object<N>` but didn't
  say recursion stops there. **Fixed** in pass: § 2.4 made
  explicit ("No recursion — a nested object's interior keys are
  not exposed"). New test 16 (NestedArgsShapeOnly) locks it.

- **MEDIUM-5 — `queryMcpTrace` not declared as a public
  surface.** The test seam declared `queryMcpTraceForTest` but
  the real provider lambda needed a non-test entry point too.
  **Fixed** in pass: § 3.1 now declares `queryMcpTrace` as
  public; test seam delegates to it.

- **LOW-1 — Sha16 determinism note.** `QJsonDocument::Compact`
  is deterministic across Qt 6.x; identical args hash equal.
  **Fixed** in pass: noted in § 2.4.

- **LOW-2 — Empty-args handling.** Confirmed `args = {}` records
  consistently: `arg_keys: {}`, `arg_bytes: 2`,
  `args_sha16` = sha("{}").left(16). **Fixed** in pass: noted in
  § 2.4 last bullet.

### Pass 2 — 2026-05-15 — MEDIUM:1

- **MEDIUM-1 — Wrap detection off-by-one at `since=0`.** Pass 1's
  fix to INV-11 said wrap iff `records[0].id > since`. With
  `since=0` and a ring exactly full but not yet wrapped (ids
  1..200), `records[0].id == 1 > 0` would falsely signal a
  wrap. **Fixed** in pass: wrap iff `records[0].id >
  max(since, 1)`, with the `max(., 1)` guarding the boundary
  (ids start at 1). Test 13 expanded with a boundary co-test:
  exactly 200 records + `since=0` does NOT trigger wrap
  detection. Both INV-11 and § 2.5 cursor-semantics text updated.

### Pass 3 — 2026-05-15 — LOW:1, otherwise CLEAN

Walked every INV-11 wrap-detection case (since=0 fresh, since=0
wrapped, since=5 fresh, since=5 wrapped, since=100 partial,
since=0 limit-clipped). All resolve correctly under
`records[0].id > max(since, 1)`. No further bugs in the
cursor logic.

- **LOW-1 — `arg_keys` int vs. double tag heuristic
  underspecified.** Qt's `QJsonValue` stores all numerics as
  double; distinguishing int requires a roundtrip check.
  **Fixed** in pass: § 2.4 now spells out the heuristic
  (`toDouble() == toLongLong()` ⇒ `"int"`, else `"double"`) and
  flags it as best-effort, not a semantic invariant.

### Pass 4 — 2026-05-15 — CLEAN

Full read of all 12 INVs, 16 tests, control-flow snippets,
and § 2.5 cursor semantics. No remaining critical / high /
medium / low findings. Spec ready for implementation.
