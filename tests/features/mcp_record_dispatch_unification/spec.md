# mcp_record_dispatch_unification — ANTS-1402

See `docs/specs/ANTS-1402.md`.

## Test scope

Source-scrape regression locks the `recordDispatch` hook
point and the success/failure call-site collapse.

## Invariants checked

- **INV-1.** `recordDispatch` declared in
  `claudeintegration.h` with the right shape.
- **INV-2.** Body in `claudeintegration.cpp` gates
  `m_tokenUsage.recordCall` on `result == "ok"` and calls
  `recordMcpTrace` unconditionally.
- **INV-3.** Dispatch success branch emits one
  `recordDispatch(...)` call (anchor `ANTS-1402-INV-3`)
  and zero standalone `m_tokenUsage.recordCall(...)` calls
  inside the `toolHandled` block.
- **INV-4.** Dispatch failure branch emits one
  `recordDispatch(...)` with `"tool_not_found"` literal
  (anchor `ANTS-1402-INV-4`).
- **INV-5.** Anchor comments present at recordDispatch
  definition + two call sites.
