# Feature: token_usage books a dispatch by what actually happened

## Problem

`recordDispatch` is the single observation point for a tool dispatch.
It derives `succeeded = (result == "ok")` and hands that to
`TokenUsageEngine::recordCall`, which routes the byte counts into
success or failed accumulators. ANTS-1432 added those failed
accumulators to measure waste on failure, after a contributor session
reported "MCP cost tokens for the failed query and saved none".

The derivation is wrong in both directions.

**An ETag 304 books as a failure.** When `applyEtagPattern`
short-circuits, the dispatch site sets `dispatchResult =
"etag_unchanged"`, which is not `"ok"`. A 304 is the cheapest
successful outcome the server has: the caller got exactly what it asked
for and the payload was skipped. So the largest saving is counted into
the counters that exist to measure waste.

**A handler's own refusal books as a success.** `dispatchResult` is set
only by dispatcher-level conditions — `mcp_disabled`,
`caller_cwd_required`, `tab_or_cwd_required`, `rate_limited`,
`dispatch_queue_full`, `tool_not_found`, `etag_unchanged`. A handler
that runs and returns `ok:false` in its own envelope — `bad_args`,
`bad_section`, `no_roadmap_loaded` — never touches it, so the call is
recorded as `"ok"` in both `token_usage` and `mcp_trace`. There is no
central refusal-envelope helper, so the dispatch site is the only place
this can be observed.

## Contract

Success is a named rule, not an equality against one string:
`"ok"` and `"etag_unchanged"` are successes; everything else is not.

A handler refusal MUST reach `dispatchResult` before the call is
recorded, carrying the envelope's own `code` where it has one so
`mcp_trace` names the refusal rather than reporting `"ok"`. It MUST NOT
override a dispatcher-level result, which already describes the call
more specifically.

The refusal probe is bounded by response size. A refusal envelope
carries a code, an error string and at most a short candidates or
example block, so it is small; a successful payload can be megabytes,
and parsing every one to ask a question whose answer is always "no"
would put a second full JSON parse on the dispatch path. Above the
bound the call is assumed successful — which is today's behaviour, so
the bound can only under-report, never mis-report.

Both rules are exposed as statics so they can be tested without driving
a dispatch.

## Invariants

**INV-1 — `"ok"` is a success.**

**INV-2 — `"etag_unchanged"` is a success.**

**INV-3 — a dispatcher refusal is not a success.** Checked for
`rate_limited`, `caller_cwd_required`, `tab_or_cwd_required`,
`tool_not_found`, `mcp_disabled`, `dispatch_queue_full`.

**INV-4 — an unknown result is not a success.** The rule is a
whitelist; a result added later defaults to failure rather than
silently counting as success.

**INV-5 — a refusal envelope yields its code.** `{"ok":false,
"code":"bad_args"}` yields `bad_args`.

**INV-6 — a refusal with no code still yields a refusal.** It must not
read as success merely because the envelope omitted `code`.

**INV-7 — a successful envelope yields no refusal.** Both `ok:true`
and an envelope with no `ok` field at all.

**INV-8 — a non-JSON or non-object body yields no refusal.** A handler
that returns a bare string must not be booked as a failure.

**INV-9 — an oversized body yields no refusal.** The size bound holds,
so the hot path pays nothing on a large successful payload.

## Scope

### In scope
- The two rules, as behaviour tests against the statics.

### Out of scope
- `TokenUsageEngine`'s own accumulator routing, owned by ANTS-1432 and
  its tests.
- Whether `mcp_trace` should keep a separate field for the handler
  code rather than reusing `result`. Reusing it is what makes the
  trace name the refusal today; a separate field is a schema change.
- Cached hits, which are already carried separately as `cachedHit`.

## Regression history

- **ANTS-1402:** made `recordDispatch` the single observation point.
- **ANTS-1432:** made `recordCall` fire on every dispatch with a
  success flag, so failed-call byte cost stopped being invisible. The
  flag was derived from equality with `"ok"`.
- **ANTS-1499:** added the ETag short-circuit, which set a result that
  is not `"ok"` — and so began booking every 304 as a failure.
- **ANTS-4457 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "`token_usage` wrong in both directions: a 304 books as a failed
  call, and a handler returning its own `ok:false` never updates
  `dispatchResult`". Verified against source in both directions and
  fixed. Locked by this spec.
