# ANTS-1360 — MCP debug-log tap (`mcp_trace` tool)

**Status:** spec, ship-ready after cold-eyes pass.
**Source:** indie-review-2026-05-14 (self-observed).
**Sibling:** ANTS-1284 (per-tool byte counters in `TokenUsageEngine`).
ANTS-1284 is a per-tool *histogram* (sum bytes / call counts);
this spec is a per-call *sequence* (id-ordered records).
They co-record at the same dispatch site but answer different
questions.

## 1. Problem

Third-party developers integrating against `mcp__ants__*` have no
way to inspect the raw request/response pairs Ants Terminal saw
without rebuilding with `ANTS_DEBUG=claude` and re-launching to
stderr. `DebugLog::Claude` writes only to
`~/.local/share/ants-terminal/debug.log` and stderr — it isn't
queryable via the MCP surface itself, and the user has to know to
enable it before the bug repeats.

For a third-party MCP client (e.g. a Lua plugin, a future
`ants-helper` subagent, or a developer's own bridge), the
quickest debug loop is:

1. Trigger the bug.
2. Ask Ants over MCP: "what tool calls did you see in the last few
   seconds?"
3. Diff against what the client thinks it sent.

That single round-trip eliminates the rebuild-and-relaunch cycle.

**Why not just read `debug.log`?**

- The log is unstructured text — every parser is bespoke.
- It's gated on the env var; if it wasn't set before the bug, the
  evidence is gone.
- It contains *all* Claude categories interleaved (transcript
  parsing, hook events, MCP dispatch).
- The file is per-process and not addressable through MCP, so a
  remote client can't fetch it.

A structured in-memory ring buffer with an MCP read tool fixes
all four.

## 2. Design

### 2.1 Where the ring lives

`ClaudeIntegration` owns the ring. Recording happens at the
dispatch site in `onMcpConnection`'s `tools/call` branch
(`claudeintegration.cpp:2469–2563`), right next to the existing
`m_tokenUsage.recordCall` call (one is per-tool histogram, the
other is per-call sequence — same site, same single hook
point).

Rationale: same site as ANTS-1284 → no second dispatch refactor;
the entire MCP dispatch is on the Qt main thread (QLocalSocket
`readyRead` slot) so no synchronisation is needed; the ring's
test seam piggy-backs on the existing ClaudeIntegration test
constructor pattern.

#### 2.1.1 Timer placement

A `QElapsedTimer t; t.start();` runs **before** the
ANTS-1357 cache lookup (line 2493 in current source). This
captures cache-hit dispatches at their true latency (typically
1–5 µs) instead of zero. The timer stops right before the
record call:

```cpp
const qint64 durationUs = t.nsecsElapsed() / 1000;
recordMcpTrace(toolName, argsObj, argBytes, respBytes,
               durationUs, cachedHit,
               toolHandled ? "ok" : "tool_not_found");
```

The single call lives at the end of the dispatch block,
after the wrap + recordCall + envelope-emit pattern, so
both branches (success and tool_not_found) reach it.

#### 2.1.2 Registration location

`registerToolProvider("mcp_trace", ...)` lives in
`MainWindow::setupClaudeIntegration` alongside the existing 25
provider registrations (`mainwindow.cpp:3727+`). The lambda
captures `[ci = m_claudeIntegration]` and calls a public
`ClaudeIntegration::queryMcpTrace(since, limit)` method —
the same helper the test seam uses (no duplication). All
provider lambdas already live in MainWindow; keeping
`mcp_trace` there matches the convention rather than scattering
registrations across two classes.

### 2.2 Ring buffer structure

```cpp
struct McpTraceRecord {
    quint64     id          = 0;     // monotonic, starts at 1
    qint64      tsMs        = 0;     // QDateTime::currentMSecsSinceEpoch
    QString     tool;                 // tool name verbatim
    QJsonObject argKeys;              // shape only: { k: "string" | "int" | ... }
    qint64      argBytes    = 0;     // compact JSON length of original args
    QString     argsSha16;            // 16-hex sha256 of compact args JSON
    qint64      respBytes   = 0;     // wrapped response length
    qint64      durationUs  = 0;     // QElapsedTimer::nsecsElapsed()/1000
    bool        cacheHit    = false; // ANTS-1357 short-circuit fired?
    QString     result;               // "ok" | "tool_not_found"
};

QList<McpTraceRecord> m_mcpTraceRing;  // bounded, oldest-first
quint64               m_mcpTraceNextId = 1;
static constexpr int  kMcpTraceCap     = 200;
```

Layout: oldest-first (push back, pop front when over cap). Read
walks from back to find ids > `since` in O(min(cap, returned)).
A separate LRU table isn't needed — the eviction policy is FIFO,
the natural shape for a debug tape.

### 2.3 What gets recorded — and what does NOT

**Recorded:** every `method == "tools/call"` dispatch, both
success (`result == "ok"`) and unknown-tool failure
(`result == "tool_not_found"`). One record per dispatch.

**NOT recorded:**

| Skip class                       | Reason |
|----------------------------------|--------|
| `method != "tools/call"`         | Out of scope; `tools/list` / `initialize` / `notifications/*` are not user-meaningful calls |
| `toolName == "mcp_trace"`        | Self-poisoning: a polling debugger fills the ring with its own queries (INV-5) |
| Malformed JSON-RPC envelopes     | Already rejected before `toolName` is parsed |
| Notifications (no `id` field)    | Already short-circuit at envelope handling |
| Late-arriving disconnections     | The dispatch site already returns early on `disconnectFromServer`; no record |

### 2.4 Argument redaction (INV-6)

The ring **never stores raw argument values**. For each call:

- `arg_keys` records *shape only*: a JSON object mapping each
  **top-level** key to a type tag in
  `{"string", "int", "double", "bool", "null", "array<N>",
  "object<N>"}` where `N` is the array length / object key count.
  **No recursion** — a nested object's interior keys are not
  exposed; only its top-level type and arity. Bounded
  recording cost: `arg_keys` size ≤ number of top-level args.
  Qt's `QJsonValue` stores all numerics as double; the
  implementation reports `"int"` when
  `value.toDouble() == static_cast<double>(value.toVariant().toLongLong())`
  (i.e. an integral value with no fractional part), else
  `"double"`. The distinction is best-effort debug
  information, not a semantic invariant.
- `arg_bytes` records the byte length of the compact-JSON form of
  the original args (the *post-parse* `arguments` object).
- `raw_bytes` (ANTS-2135) records the size of the *raw inbound
  JSON-RPC frame* (`buf.size()` at the socket), as distinct from
  `arg_bytes`. It is `-1` for non-socket callers (the test seam, and
  any future caller without a frame). It is the discriminator for the
  large-body write drop (ANTS-2135/2142): a refused call with
  `arg_bytes: 2` (an empty `{}` arrived) and a SMALL `raw_bytes` means
  the body never reached Ants — an upstream Claude-Code serialisation
  drop, not an Ants bug (confirmed by ANTS-1853); a LARGE `raw_bytes`
  alongside `arg_bytes: 2` would instead implicate an Ants-side parse
  loss. The framing has no length-prefix, so `raw_bytes` is the only
  in-ring signal of how much actually crossed the socket.
- `args_sha16` is the first 16 hex chars of the SHA-256 of the
  compact-JSON form of the original args — correlates repeat
  calls without leaking values. `QJsonDocument::Compact`
  is deterministic across Qt 6.x, so identical
  `(tool, args)` pairs always hash equal.
- Empty `args` (`{}`) record `arg_keys: {}`, `arg_bytes: 2`,
  `args_sha16: <hash of "{}">`.

Rationale: paths and command text passed to `send-text`,
`workspace_search`, `launch`, and `file_outline` can contain
user secrets, partial PII, or scratch text the user does not
want a third-party MCP client to read. Storing only shape +
length + hash gives debuggers the dispatch sequence they need
without exposing payloads.

Worked example — a `send-text { tab_id: 3, text: "..." }`
dispatch records:

```json
{"id":42,"ts_ms":1747100000000,"tool":"send-text",
 "arg_keys":{"tab_id":"int","text":"string"},
 "arg_bytes":58,"raw_bytes":140,"args_sha16":"a3f2d8e1b0c4f95e",
 "resp_bytes":24,"duration_us":210,
 "cache_hit":false,"result":"ok"}
```

— enough to debug "was the call made, with the right shape, at
the right time?" — not enough to read the text.

### 2.5 MCP tool surface

```json
{
  "name": "mcp_trace",
  "description":
    "Return the last N tool/call records observed by this Ants Terminal MCP server. Useful for debugging third-party MCP integrations. Records are structured (shape + lengths + hashes only) — raw argument values are never stored. ANTS-1360.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "since": {
        "type": "integer", "minimum": 0,
        "description":
          "Return only records with id > since. Use 0 to start from the oldest record currently in the ring."
      },
      "limit": {
        "type": "integer", "minimum": 1, "maximum": 200, "default": 50,
        "description":
          "Maximum records returned. Hard cap = ring cap (200)."
      }
    },
    "additionalProperties": false
  }
}
```

**Response shape:**

```json
{
  "ok": true,
  "records": [ ... oldest-to-newest, ids strictly increasing ... ],
  "next_id": <cursor for the next poll>,
  "ring_capacity": 200,
  "ring_size": <current record count in the ring>
}
```

**Cursor semantics:**

- Filter on read is `id >= since` (inclusive lower bound).
- `next_id` = `max(returned_record.id) + 1` when records are
  returned; `next_id == since` (input echoed) when no records
  match.
- A client polling with `since = previous next_id` therefore
  walks the trace without skipping any records and without
  re-receiving any record.
- A ring wrap (the oldest records were evicted before the client
  observed them) is detectable when `ring_size ==
  ring_capacity AND records[0].id > max(since, 1)`. The
  `max(., 1)` guards the boundary case `since=0` on a
  not-yet-wrapped full ring. In that case the client missed
  `records[0].id - max(since, 1)` records and should treat the
  gap as an explicit drop, not a deliverable record. (Full
  INV-11.)

### 2.6 Cache interaction (ANTS-1357)

`mcp_trace` is **NOT** in the `kIdempotentReadTools` allowlist.
Two reasons:

1. The ring mutates on every dispatch, so a 100 ms cache hit
   would mask new records — exactly the opposite of the tool's
   purpose.
2. The typical query pattern is `since = previous_next_id`,
   which already varies per call, so the cache would mostly
   miss anyway.

Conversely: when an allowlisted tool's cache *does* fire, the
trace records `cache_hit:true` so the developer can see the
short-circuit fired without having to instrument ANTS-1357
separately.

### 2.7 Wrapping (ANTS-1294)

`mcp_trace` responses pass through `wrapMcpData`. The wrap is
correct here: the records contain tool names and arg-shape
data that originated from a (possibly untrusted) MCP client,
so framing the payload as `<ants_mcp_data tool="mcp_trace">`
prevents an embedded record from being interpreted as
instructions by the consuming assistant.

### 2.8 Invariants

- **INV-1 (FIFO ring)** Cap = 200. On insert past cap, the
  oldest record (`m_mcpTraceRing.front()`) is removed.
- **INV-2 (monotonic ids)** `id` starts at 1, increments by 1
  on each insert, never resets within a process lifetime,
  never re-used after eviction.
- **INV-3 (since semantics)** `mcp_trace(since=S)` returns
  records with `id >= S`, sorted ascending by id. `since=0`
  returns the whole ring (subject to `limit`). `next_id` is
  `max(returned.id) + 1` on non-empty response, else equal to
  the `since` input.
- **INV-4 (limit cap)** Returned count `= min(limit, available)`
  where `available = count(id >= since)`. `limit` clamps to
  `[1, ring_capacity]`; out-of-range values are clamped, not
  rejected.
- **INV-5 (no self-recording)** A dispatch whose
  `toolName == "mcp_trace"` MUST NOT insert a record.
- **INV-6 (no value leak)** No raw argument value is ever
  stored in a record. Only shape (`arg_keys`), byte length
  (`arg_bytes`), and hash (`args_sha16`).
- **INV-7 (dispatch coverage)** Every successful `tools/call`
  dispatch AND every unknown-tool failure produces exactly
  one record. Notifications (no id), malformed envelopes,
  and `tools/list` / `initialize` MUST NOT produce records.
- **INV-8 (cache_hit truthfulness)** `cache_hit:true` iff the
  ANTS-1357 short-circuit fired for this dispatch. Future
  control-plane tools that bypass the cache continue to
  record `cache_hit:false`.
- **INV-9 (RAM bound)** With 200 records × ~250 B
  (worst-case per-record JSON-equivalent), ring memory ≤ ~50 KB
  on a 32 GiB host budget — comfortably below the existing
  `m_idempotentReadCache` (~19 KB) and well below
  `m_roadmapCacheBullets` (~85 KB).
- **INV-10 (single-threaded)** The ring is accessed only from
  the Qt main thread (QLocalSocket `readyRead` slot). No
  mutex; a documented invariant in the header.
- **INV-11 (next_id continuity)** A polling client using
  `since = previous next_id` sees no gaps unless the ring
  wrapped (i.e. evicted entries the client hadn't yet seen).
  Wrap detection: `ring_size == ring_capacity AND
  records[0].id > max(since, 1)`. The `max(., 1)` accounts for
  the boundary case `since=0` on a not-yet-wrapped full ring
  (where `records[0].id == 1` does NOT imply a wrap; ids start
  at 1). When the condition holds, the ring evicted
  `records[0].id - max(since, 1)` records the client hadn't
  read; the gap is intentional and detectable.
- **INV-12 (failure-path bytes)** Records with
  `result == "tool_not_found"` carry `resp_bytes = 0` (no
  response was emitted) and a valid (small, microsecond-range)
  `duration_us`.

### 2.9 RAM budget detail

Each record's runtime footprint:

| Field         | Bytes |
|---------------|------:|
| `id`          |    8 |
| `tsMs`        |    8 |
| `tool`        |  ~40 (`QString` overhead + ~16 UTF-16 cp) |
| `argKeys`     |  ~80 (QJsonObject with 2-4 small string keys) |
| `argBytes`    |    8 |
| `argsSha16`   |  ~48 (16-char QString + overhead) |
| `respBytes`   |    8 |
| `durationUs`  |    8 |
| `cacheHit`    |    1 |
| `result`      |  ~32 |
| Slack/align   |  ~10 |
| **Per-record**| **~250 B** |

Cap 200 → **~50 KB ceiling**. Peer comparisons:

- `m_idempotentReadCache` cap 32 × 600 B = ~19 KB.
- `m_planModeByPid` is sparse; effectively a handful of bytes.
- `m_changedFiles` grows with /audit-touched files (unbounded
  in scope, capped in practice at low hundreds).

Recording one record is `O(1)` (push_back + pop_front if over
cap). Reading is `O(min(cap, returned))`.

## 3. Test surface

The dispatch site lives inside the `onMcpConnection` lambda
which is not directly callable. The same trade as ANTS-1357
applies: test through a direct cache API on `ClaudeIntegration`
that bypasses the QLocalSocket round-trip.

### 3.1 Test seam + public surface

Public methods on `ClaudeIntegration`:

```cpp
// Called by the registered mcp_trace provider lambda.
QJsonObject queryMcpTrace(quint64 since, int limit) const;
```

Test-only methods (call the same private helpers
`recordMcpTrace` / `queryMcpTrace` that the dispatch site and
the public method use):

```cpp
void recordMcpTraceForTest(
    const QString &toolName, const QJsonObject &args,
    qint64 argBytes, qint64 respBytes, qint64 durationUs,
    bool cacheHit, const QString &result);

QJsonObject queryMcpTraceForTest(quint64 since, int limit) const {
    return queryMcpTrace(since, limit);
}

int  mcpTraceSizeForTest() const;
void clearMcpTraceForTest();
```

Integration with `processTools` rides on code review of the
small dispatch-site diff (4–6 LOC) — same trade as ANTS-1357.

### 3.2 Tests (`tests/features/mcp_trace_ring_buffer/`)

1. **EmptyRingReturnsEmpty** — query (`since=0, limit=50`) on a
   fresh integration returns `{records:[], next_id:0,
   ring_size:0, ring_capacity:200}`.
2. **SingleRecordPreservesShape** — record one `get_cwd` call;
   query returns 1 record with `tool=="get_cwd"`, `arg_keys`
   containing the right keys with type tags, `result=="ok"`,
   `id==1`.
3. **MonotonicIds** — record 5 calls; ids are 1..5 in insertion
   order; `next_id==6`.
4. **CapEvictsOldest** — record 250 calls; ring_size==200;
   `records[0].id == 51` after a `since=0, limit=200` query.
5. **SinceFilter** — record 10 calls; query `since=5, limit=50`
   returns ids 5..10 (inclusive lower bound per INV-3).
   Subsequent query `since=11, limit=50` returns empty;
   `next_id == 11`.
6. **LimitClampsCount** — record 20 calls; query `since=0,
   limit=3` returns 3 records (ids 1..3).
7. **LimitMinClamp** — query `since=0, limit=0` is clamped to
   limit=1 (returns 1 record); query `limit=999` is clamped to
   200.
8. **NoSelfRecording** — call `recordMcpTrace(tool="mcp_trace",...)`;
   `mcpTraceSizeForTest()` stays unchanged.
9. **UnknownToolStillRecorded** — record one call with
   `result="tool_not_found"`; queryable, ids monotonic.
10. **ArgRedactionShapeOnly** — record `send-text` with
    args `{"tab_id":3, "text":"secret"}`; the resulting record's
    `arg_keys` is `{"tab_id":"int","text":"string"}` and no
    field contains the literal `"secret"`.
11. **ArgsSha16IsDeterministic** — record the same `(tool,
    args)` pair twice; both records carry identical
    `args_sha16`.
12. **CacheHitFlagPropagates** — record with `cacheHit=true`;
    query returns `cache_hit:true`.
13. **WrapDetectableViaRingSize** — record 250 calls; query
    `since=0` returns 200 records; `ring_size==ring_capacity
    AND records[0].id > max(since, 1)` (wrap detection,
    INV-11). Boundary co-test: record exactly 200 calls,
    query `since=0` — `ring_size==ring_capacity` is true but
    `records[0].id == 1 == max(since, 1)`, so wrap is NOT
    detected (the ring is full-but-pristine).
14. **NextIdContinuityWithoutWrap** — record 10 calls; query
    `since=0, limit=100`; reissue with `since = next_id`;
    returns empty until next record arrives, at which point
    the new record appears with `id == next_id`.
15. **FailurePathRespBytesZero** — record with
    `result="tool_not_found"` and `respBytes=0`; query confirms
    the record carries `resp_bytes:0` and a non-negative
    `duration_us` (INV-12).
16. **NestedArgsShapeOnly** — record with args
    `{"outer":{"inner":"secret","count":3}}`; `arg_keys` is
    `{"outer":"object<2>"}` and no field contains the literal
    `"secret"` or `"inner"` (INV-6 no-recursion).

### 3.3 Out of scope for the test bundle

- End-to-end through the QLocalSocket round-trip. The MCP
  schema test bundle (`tests/features/mcp_tools_list_schema/`)
  will gain one assertion that `mcp_trace` appears in
  `tools/list` — that exercises wire-side schema correctness
  without needing a full dispatch lane.

## 4. Build cost

- ~120 LOC added to `claudeintegration.cpp`:
  - `recordMcpTrace`, `queryMcpTrace`, `argShapeOf` helpers (~80 LOC)
  - 4–6 LOC at the dispatch site (one helper call + one cache_hit
    forward)
  - `registerToolProvider("mcp_trace", ...)` registration in the
    setup path (~20 LOC)
- ~30 LOC added to `claudeintegration.h` (members + test seam + cap).
- 1 LOC added to `CMakeLists.txt` (new test target under the
  `test_claude` bundle).
- New test target: ~280 LOC C++ + spec.md.

## 5. Out of scope

- **Persistence across restarts.** Ring is in-memory only.
  Matches `DebugLog`'s lifetime; restart wipes both.
- **Raw arg value recording.** INV-6 forbids it. Developers
  needing value-level traces enable `ANTS_DEBUG=claude` and
  read `debug.log` — privileged user-machine access only.
- **Per-tool filtering server-side.** Client filters
  `records[*].tool` itself; keeps the tool surface small.
- **Cross-process ring.** Each `ants-terminal` process has its
  own ring. Two Ants instances under different UIDs would
  expose separate traces.
- **Recording other JSON-RPC methods** (`tools/list`,
  `initialize`, notifications). Future ANTS-1xxx can extend
  the ring to cover those if a real debug need emerges.

## 6. Migration

None. New tool surface; existing dispatch behaviour unchanged
except for a 4–6 LOC instrumentation hook.

## 7. Discoverability

- `tools/list` registers `mcp_trace` alongside the existing 25+
  tools. The schema description carries the ANTS-1360 tag so a
  Claude debugging an integration can grep for the spec.
- A one-line addition under the `claudeintegration` bullet in
  the root `CLAUDE.md` "Module map" section calls out the new
  ring buffer and the `mcp_trace` MCP verb (matching how
  ANTS-1294 / ANTS-1295 are referenced today).
- CHANGELOG entry under `[Unreleased]` describes the new tool
  and its arguments.
- ROADMAP entry flips from 📋 to ✅ with the standard
  shipped-detail body.

## 8. Cold-eyes review log

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
