# Feature: `CallerCwdContract::TabSpecific` enforcement (ANTS-1415, Phase 3b)

Human contract for the source-scrape regression test in this
directory. Full design: `docs/specs/ANTS-1415.md`.

Phase 3a (ANTS-1404) enforced the `Required` caller-cwd contract at the
MCP `tools/call` dispatch site and classified six tools `TabSpecific`
without enforcing them. Phase 3b enforces `TabSpecific`: a per-tab read
tool is refused when the caller supplies no usable routing key, so it
no longer silently falls back to the focused terminal tab (a
cross-tenant data leak).

The six TabSpecific tools: `get_text`, `recent_errors`,
`get_scrollback`, `get_last_command`, `get_environment`, `get_cwd`.
Only `get_text` and `recent_errors` honour an explicit `tab` index; the
other four route on `caller_cwd` only.

## Invariants under test (source-scrape)

- **INV-1.** `docs/standards/mcp-error-codes.md` § 3 carries the
  `tab_or_cwd_required` row.
- **INV-2.** The `tools/call` dispatch refuses a `TabSpecific` tool
  with `code:"tab_or_cwd_required"` on
  `contract == CallerCwdContract::TabSpecific` and the
  `callerCwd.isEmpty() && !hasTab` condition.
- **INV-3.** The `tab_or_cwd_required` refusal appears AFTER the
  `caller_cwd_required` (Phase 3a) refusal and BEFORE the
  `isIdempotentReadTool` cache gate — so it runs before the cache.
- **INV-4.** `tabSpecificAcceptsTabIndex` is defined, returns true for
  `get_text` and `recent_errors`, and does NOT list the four cwd-only
  tools.
- **INV-8.** The refusal branch sets
  `dispatchResult = QStringLiteral("tab_or_cwd_required")` so
  `recordDispatch` counts it as a failed call.
- **INV-9.** `callerCwdContractFor` still classifies all six tools as
  `CallerCwdContract::TabSpecific` (no reclassification).
