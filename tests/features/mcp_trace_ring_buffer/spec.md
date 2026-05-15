# ANTS-1360 — `mcp_trace` ring buffer conformance

Behavioural feature test for the per-call MCP dispatch trace.
Spec: `docs/specs/ANTS-1360.md`.

## Invariants covered

- **INV-1 (FIFO eviction at cap)** — record past
  `kMcpTraceCap` evicts the oldest. (CapEvictsOldest)
- **INV-2 (monotonic ids)** — ids start at 1, increment by 1,
  never reuse. (MonotonicIds)
- **INV-3 (since inclusive)** — `query(since=S)` returns records
  with id >= S; `next_id` echoes `since` on empty result.
  (SinceFilter, NextIdContinuityWithoutWrap)
- **INV-4 (limit clamp)** — `limit < 1` clamps to 1; `limit >
  cap` clamps to cap. (LimitClampsCount, LimitMinClamp)
- **INV-5 (no self-recording)** — `recordMcpTrace(tool="mcp_trace",...)`
  leaves ring untouched. (NoSelfRecording)
- **INV-6 (no value leak)** — only shape + lengths + hash;
  nested objects report `object<N>` without recursion.
  (ArgRedactionShapeOnly, NestedArgsShapeOnly)
- **INV-7 (dispatch coverage)** — unknown-tool records too.
  (UnknownToolStillRecorded)
- **INV-8 (cache_hit propagates)** — recorded flag matches input.
  (CacheHitFlagPropagates)
- **INV-9 (RAM bound)** — implicit via cap test.
- **INV-10 (single-threaded)** — defensive invariant, not
  tested directly.
- **INV-11 (wrap detection)** — `ring_size == ring_capacity AND
  records[0].id > max(since, 1)` flags an eviction gap; boundary
  case (exactly-full ring + since=0) does NOT flag.
  (WrapDetectableViaRingSize)
- **INV-12 (failure-path bytes)** — `resp_bytes:0` + non-negative
  `duration_us`. (FailurePathRespBytesZero)
- **args_sha16 determinism** — same args hash equal across
  records. (ArgsSha16IsDeterministic)
- **Single-record shape** — fields populated as spec'd.
  (SingleRecordPreservesShape)
- **Empty ring** — query returns empty list + zero ring_size.
  (EmptyRingReturnsEmpty)

## Out of scope

- End-to-end through the QLocalSocket dispatch lambda (same
  trade as ANTS-1357 — test the direct API).
- ANTS-1294 `<ants_mcp_data>` wrapping (already locked by the
  output-sanitisation test bundle).
