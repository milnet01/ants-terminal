# Feature spec: session_memory + project_layout read-op tenancy split (ANTS-1435)

Vestige CC session 2026-05-16 reported `session_memory op:"list"`
refusing with `cwd_mismatch` even when `caller_cwd` was correctly
supplied — the focused Ants tab happened to be on a different
project. The whole point of `caller_cwd` (per ANTS-1391) is to
anchor the read to the caller's project. Spec ANTS-1435 split the
gate by op: reads honour `caller_cwd` directly; writes keep RcGate.

## Invariants

- **INV-1 / Read ops anchor to caller_cwd.** `cmdSessionMemory`'s
  read branch (`isReadOp`) takes caller_cwd → canonicalise → isDir
  → tenant cwd. No RcGate call on this path. Source anchor:
  `ANTS-1435 — gate routing is now ASYMMETRIC` in
  `the remotecontrol TUs::cmdSessionMemory`.
- **INV-2 / Write ops keep RcGate.** The `else` branch (op == Set
  or Delete) still calls `RcGate::checkCallerCwd`. Confirms by
  source-grep: `RcGate::checkCallerCwd` appears AFTER the
  `isReadOp` branch inside the cmdSessionMemory body.
- **INV-3 / Read-op cwd_missing.** Empty `caller_cwd` on a read op
  returns `{ok:false, code:"cwd_missing"}` with smErr 4-field shape.
- **INV-4 / Read-op cwd_bad on non-existent path.** Non-empty
  caller_cwd that canonicalises to empty returns
  `{ok:false, code:"cwd_bad"}`.
- **INV-4b / Read-op cwd_bad on non-directory.** Any existing path
  that isn't a directory (regular file, FIFO, device) returns
  `{ok:false, code:"cwd_bad"}` with a "is not a directory" message.
  Prevents `caller_cwd:"/etc/passwd"` from hashing to a real bucket.
- **INV-5 / project_layout follows same pattern.** `cmdProjectLayout`
  also canonicalises + isDir-checks caller_cwd directly; no RcGate
  body call. Source anchor: `ANTS-1404 + ANTS-1435 — caller_cwd
  anchoring` in `the remotecontrol TUs::cmdProjectLayout`.
- **INV-6 / Contract registry includes session_memory as Required.**
  `callerCwdContractFor("session_memory")` returns `Required` so
  dispatcher refuses empty `caller_cwd` upstream. Source anchor:
  `ANTS-1435 — session_memory` in
  `src/claudeintegration.cpp::callerCwdContractFor`.
- **INV-7 / Per-verb envelope shape preserved.** session_memory
  refusals from EITHER branch emit smErr's 4-field
  `{ok, code, error, op, echo}` shape; project_layout refusals
  emit the bare 3-field `{ok, error, code}`. The asymmetry
  predates 1435 and is honestly stated.
