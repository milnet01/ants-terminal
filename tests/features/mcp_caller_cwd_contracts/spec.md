# mcp_caller_cwd_contracts — ANTS-1404 Phase 3a

See `docs/specs/ANTS-1404.md`.

## Test scope

Source-scrape regression locks the contract-table classification and
the dispatch-site refusal envelope.

## Invariants checked

- **CLS-1.** `callerCwdContractFor` exists and classifies the four
  Required tools (`get_git_status`, `last_audit_summary`,
  `git_state`, `verify_changes`) as `CallerCwdContract::Required`.
- **CLS-2.** TabSpecific / ProcessGlobal groups appear in the table
  (Phase 3a documentation; no enforcement yet).
- **DISP-1.** `processTools` invokes `callerCwdContractFor(toolName)`
  before the cache lookup, and refuses with
  `caller_cwd_required` when contract is Required and caller_cwd
  is empty.
- **DISP-2.** Refusal envelope shape: `{ok:false, code:"caller_cwd_required",
  error:"<tool>: ..."}`.
- **DISP-3.** Refused calls bypass the idempotent-read cache —
  `cacheable` becomes false when `toolHandled` is set by the
  refusal.
- **DISP-4.** The `if (!toolHandled)` guard around the provider
  dispatch ensures refused tools don't invoke the provider lambda.
