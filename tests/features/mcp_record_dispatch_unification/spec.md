# mcp_record_dispatch_unification — ANTS-1402

See `docs/specs/ANTS-1402.md`.

## Test scope

Source-scrape regression locks the `recordDispatch` hook
point and the success/failure call-site collapse.

## Invariants checked

- **INV-1.** `recordDispatch` declared in
  `claudeintegration.h` with the right shape.
- **INV-2.** Body in `claudeintegration.cpp` derives
  `m_tokenUsage.recordCall`'s success flag from `result`, and calls
  `recordMcpTrace` unconditionally.

  Amended twice, and the original wording was stale before the first
  amendment. It said recordCall is *gated* on `result == "ok"`;
  ANTS-1432 removed that gate, because skipping the call on failure
  made failed-call byte cost invisible — recordCall now fires on every
  dispatch and the engine routes the bytes by the flag. ANTS-4457 then
  moved the comparison itself into `dispatchResultIsSuccess`, so the
  rule could be unit-tested and so `etag_unchanged` could join `ok` as
  a success. The property this invariant protects — the flag comes from
  `result` rather than being a constant — is unchanged by both. The
  acceptance check takes either spelling; see
  `tests/features/dispatch_result_accounting/spec.md`.
- **INV-3.** Dispatch success branch emits one
  `recordDispatch(...)` call (anchor `ANTS-1402-INV-3`)
  and zero standalone `m_tokenUsage.recordCall(...)` calls
  inside the `toolHandled` block.
- **INV-4.** Dispatch failure branch emits one
  `recordDispatch(...)` with `"tool_not_found"` literal
  (anchor `ANTS-1402-INV-4`).
- **INV-5.** Anchor comments present at recordDispatch
  definition + two call sites.
