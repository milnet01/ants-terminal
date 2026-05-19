# Test-Audit Resume Recipe

**Status**: v1 (ANTS-1580 — pull 32, 2026-05-19)
**Audience**: Claude Code sessions / non-CC orchestrators that pick
up a partially-completed `/test-audit` after the initial session
exited.

## Why this exists

`test_audit_partition` returns a `partition_token` that
`test_audit_brief`, `test_audit_synthesis_prompt`, and
`test_audit_fold_in` accept as a handle to the same audit run. The
token is generated from canonical-cwd + scope + dimensions + the
max mtime in the scoped tree (per `testauditengine.cpp:982`).

The token is **held in an in-process LRU cache** inside the Ants
binary (`g_partitionCache` in `testauditengine.cpp:183–187`). It
does **not** persist to disk and is **not** auto-saved into
`session_memory`. Two consequences:

1. **Ants restarts wipe every token.** After a `pkill ants-terminal`
   or a system reboot, every partition_token from prior sessions is
   gone and `test_audit_brief` will refuse with
   `{ok:false, error:"... partition_token \"X\" not found — re-run
   partition"}`. The only recovery is a fresh
   `test_audit_partition` call.

2. **File changes inside the scoped tree invalidate the token.**
   The qHash mix includes the max mtime; touching any file in
   scope changes the token, so a token saved at 09:00 is invalid
   at 14:00 if anything in the tree was edited. This is
   conservative-correct: the chunk packing reflects the file set
   at partition time, and re-running partition is cheap.

## What "resume" actually looks like

The realistic resume pattern is **"save the token explicitly, accept
that a partition re-run is the common path"**:

### Saving the token (after partition)

```jsonc
session_memory(
  op:"set",
  key:"test_audit_partition_token:<scope_id>",
  value:{
    token: "<the token from test_audit_partition response>",
    scope: "<the scope arg you passed>",
    dimensions: "<the dimensions arg you passed>",
    saved_at_ms: <epoch ms>
  }
)
```

The key naming convention `test_audit_partition_token:<scope_id>`
lets a single project have multiple audits in flight (per scope —
e.g. `tests/api` vs `tests/integration`). Use a short
human-readable `scope_id` you can recall in a follow-up session.

### Resuming (in a later session)

1. Read the saved entry:

   ```jsonc
   session_memory(op:"get", key:"test_audit_partition_token:<scope_id>")
   ```

2. Try `test_audit_brief` with the returned token. If it succeeds,
   the partition cache is still warm and you can proceed straight
   to Phase 2.

3. If `test_audit_brief` refuses with `partition_token not found`,
   re-run `test_audit_partition` with the same `scope` +
   `dimensions` args. The token will differ if any file changed;
   overwrite the saved entry under the same key.

### Lifetime budget

The in-process cache holds ~64 partitions LRU. A long-lived Ants
session running 5 audits a day evicts old tokens quickly. Treat
the saved entry as a **convenience pointer, not a guarantee** —
the recipe above always falls back to re-running partition.

## When a `test_audit_resume` verb would help

A first-class verb (`test_audit_resume({partition_token | latest:true})`)
would make this easier for orchestrators that don't want to write
the session_memory handshake themselves. That's tracked as a v2
follow-up on ANTS-1580. v1 leaves the recipe at the
client-orchestration layer because:

- The partition cost is small (≤ 5 s on a 1 K-file suite) — the
  client-side recipe doesn't materially save wall-clock.
- session_memory writes are cheap and explicit; users who care
  about resume already use it for other state.
- A new verb that depends on durable partition storage on disk
  would also need eviction, GC, and per-project quota — non-trivial
  to add for a workflow whose primary cost is the subagent calls,
  not the partition.

## Related

- `docs/specs/ANTS-1397.md` — the test_audit trio's v1 contract.
- `docs/standards/audit-false-positives.md` — the FP ledger that
  carries across audit runs (orthogonal to partition_token; this
  one IS durable on disk).
- `session_memory` MCP tool — the durable per-cwd KV used here.
