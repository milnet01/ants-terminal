# Feature spec: MCP Required-contract baseline (ANTS-1416)

ANTS-1336 + ANTS-1404 + ANTS-1435 established the dispatcher-level
`CallerCwdContract::Required` gate as the canonical refusal path
for tools that mutate per-project state under the caller's project
root. This test pins the security-critical Required group so a
future refactor can't silently downgrade a tool from `Required` to
`Optional` (which would re-introduce the confused-deputy attack the
gate was built to close).

Out of scope: the bullet's original "drop the handler-level RcGate
call" goal turned out to be unsafe — `cmdSessionMemory` is reachable
via both the MCP dispatcher AND the JSON-RPC IPC socket
(`remotecontrol.cpp::handleConnection`), and the IPC path doesn't
go through `callerCwdContractFor`. So the handler-level check stays
as IPC-path coverage; this spec locks in the dispatcher-level
contract instead.

## Invariants

- **INV-1 / session_memory is classified `Required`.**
  `ClaudeIntegration::callerCwdContractFor("session_memory") ==
  Required`. Asymmetric internal routing (reads anchor to
  `caller_cwd`, writes match focused tab via RcGate) is preserved
  per ANTS-1435; the Required dispatcher-level gate prevents
  empty-`caller_cwd` calls from reaching either routing path on the
  MCP surface.
- **INV-2 / Security-critical Required baseline.** The Required
  group must include the full set established by ANTS-1404 + later
  additions: `get_git_status`, `last_audit_summary`, `git_state`,
  `verify_changes`, `audit_run`, `project_layout`, `session_memory`,
  `roadmap_log`. A future contributor reclassifying any of these
  to `Optional` would re-open the silent-focused-fallback leak
  (2026-05-15 cross-session report) that ANTS-1404 closed.
- **INV-3 / `caller_cwd_info` is Optional, not Required.** The
  diagnostic verb takes `caller_cwd` as an INPUT (not an anchor).
  Optional preserves the "what would happen without it?" question
  the verb is built to answer.

## Test scope

Functional — exercises the static `callerCwdContractFor` helper
directly. No source-scrape; the classification function is the
authoritative source of truth and a unit-style test is cleaner
than scraping comments.
