# mcp_workflow_state — Feature Spec (ANTS-1723)

## Purpose
`workflow_state` gives superpowers skills a per-project, per-skill
scratch-pad that survives `/clear`. Skills call `op:"get"` at the start
of a turn to resume from a compact "current step" snapshot.

## Storage
Same backing file as `session_memory`:
`~/.cache/ants-terminal/mcp-state/<sha256(cwd)>.json`.
Key namespace: `wf.<skill>` (e.g. `wf.tdd`, `wf.systematic-debugging`).
Note: dot separator — slash is not valid in the key charset.
Skill name: `^[A-Za-z0-9_-]{1,32}$`.
Value shape: `{step:int, phase:str, notes:str[], updated_at_ms:float64}`.

## ANTS-1435 write-gate
`set` and `clear` ops use `RcGate::checkCallerCwd` (same as
`session_memory`). Read ops (`get`) anchor to `caller_cwd` directly.

## Invariants

- **INV-1** `op:"get"` returns `{ok:true, found:true, state:{…}}` when
  an entry exists and `{ok:true, found:false}` when absent or expired.
- **INV-2** `op:"set"` stores `{step, phase, notes, updated_at_ms}`
  (server clock overwrites `updated_at_ms`) and returns `{ok:true}`.
- **INV-3** `op:"clear"` deletes the `wf.<skill>` entry and returns
  `{ok:true, deleted:true}` if it existed, `{ok:true, deleted:false}`
  if absent.
- **INV-4** A `wf.<skill>` entry whose `updated_at_ms` is older than
  72 h is treated as absent on `get` (returns `found:false, expired:true`).
- **INV-5** On every `set`, all `wf.` keys with `updated_at_ms` older
  than 72 h are deleted from the store (lazy TTL purge).
- **INV-6** `caller_cwd` absent → refuse `{ok:false, code:"cwd_missing"}`.
  Unresolvable → `{ok:false, code:"cwd_bad"}`. (mcp-error-codes.md §input-validation.)
- **INV-7** A present-but-invalid skill name (fails `^[A-Za-z0-9_-]{1,32}$`)
  → refuse `{ok:false, code:"bad_args", error:"workflow_state: invalid skill
  name"}`. (An *absent* skill is INV-11, not this — the regex message only
  describes a non-conforming value.)
- **INV-8** `step` and `phase` are required on `set`. Missing either →
  `{ok:false, code:"bad_args"}`.
- **INV-9** Stored value payload ≤ 4 KiB (serialised). Exceeding →
  `{ok:false, code:"payload_too_large"}`.
- **INV-10** Keys stored as `wf.<skill>` (dot separator). They appear
  in `session_memory op:list` results distinguished by the `wf.` prefix.
- **INV-11** (ANTS-3511) Arg-validation refusals resolve the full required
  arg set in one round-trip: an absent `op` refuses with a message naming
  *both* required args (`"op and skill are required"`); an absent/empty
  `skill` refuses `"workflow_state: skill is required"` (distinct from the
  INV-7 regex message, which is reserved for a present-but-malformed value).
