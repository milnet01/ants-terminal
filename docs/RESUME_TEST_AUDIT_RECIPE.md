# Resuming a `/test-audit` run (ANTS-1523)

A session that picks up the working tree an earlier `/test-audit`
session left modified-but-uncommitted (FP## triage half-done, fold-in
not yet written, deferred follow-ups still sitting in
`session_memory`) has no first-class MCP entry point that announces
"this is a resume." This recipe is the documented bootstrap until a
thin `test_audit_resume` verb lands.

The recipe is conservative — it re-verifies each prior finding against
the *current* tree rather than trusting the earlier session's call.
Test smells migrate as code moves; an FP## that was a true positive
last week may have been fixed in the meantime (or vice versa).

## Step 1 — find the prior `partition_token`

The orchestrator stores its partition + per-chunk state under
`session_memory` keyed by a `partition_token` (typically derived from
the corpus mtime hash). Recover it with a wildcard list:

```jsonc
// MCP call
mcp__ants__session_memory({
  op: "list",
  caller_cwd: "<your $PWD>",
  prefix: "test_audit/"
})
```

Expect entries like:

```
test_audit/<token>/partition
test_audit/<token>/chunk/<chunk_id>/findings
test_audit/<token>/chunk/<chunk_id>/triage
test_audit/<token>/deferred
```

The newest `partition` key's `<token>` is the one to resume. If you
see more than one, sort by `wrote_at` and pick the latest unless the
user names a specific token.

## Step 2 — pull the prior partition + deferred list

```jsonc
// Fetch the partition manifest
mcp__ants__session_memory({
  op: "get",
  caller_cwd: "<your $PWD>",
  key: "test_audit/<token>/partition"
})

// Fetch any deferred FP## the prior session set aside
mcp__ants__session_memory({
  op: "get",
  caller_cwd: "<your $PWD>",
  key: "test_audit/<token>/deferred"
})
```

The partition manifest carries the same shape `test_audit_partition`
would have returned originally (`chunks[]`, `dimensions[]`, etc.).
Deferred items carry their original FP## ids and cited `file:line`.

## Step 3 — re-verify each deferred finding against current code

For every deferred finding, confirm the cited `file:line` *still*
contains the smell the prior session called out. This is the step
that prevents resuming with stale evidence:

```jsonc
// One verify-call per finding (or batched if you have many)
mcp__ants__workspace_search({
  caller_cwd: "<your $PWD>",
  pattern: "<the original smell anchor — e.g. \"sleep(\"",
  lane: "<chunk root or path from finding>",
  case: "sensitive",
  context: 1
})
```

Compare the returned line to the finding's cited line:

| Re-check outcome                                    | Action                                                                    |
|-----------------------------------------------------|---------------------------------------------------------------------------|
| Smell still present at cited file:line              | Keep in the resume queue.                                                 |
| Smell present at a different line in the same file  | Update the finding's `line` and keep.                                     |
| Smell absent (fixed since)                          | Drop from the resume queue. Log to `session_memory` under `…/resolved/`.  |
| File deleted/renamed                                | Drop. The fold-in step would emit a dangling reference.                   |

The `git_state` verb is the cheapest cross-check for renames:

```jsonc
mcp__ants__git_state({
  caller_cwd: "<your $PWD>",
  op: "log",
  n: 30,
  path: "<the cited file>"
})
```

## Step 4 — run the synthesis + fold-in on the surviving set

Once the re-verified set is in hand, call the standard fold-in path:

```jsonc
// 1. Render the synthesis prompt for the surviving findings.
mcp__ants__test_audit_synthesis_prompt({
  caller_cwd: "<your $PWD>",
  // shape mirrors what the original orchestrator passed
})

// 2. After the synthesis subagent returns, fold into ROADMAP.
mcp__ants__test_audit_fold_in({
  caller_cwd: "<your $PWD>",
  actionable: [/* the corroborated, re-verified findings */],
  date_iso: "<today>",
  // partition_token: "<token>"  // when the verb gains an idempotent
                                  // gate (ANTS-1527 follow-up)
})
```

`test_audit_fold_in` allocates IDs from `.roadmap-counter` and writes
the block atomically — the same path the original session would have
taken, just with a hand-curated resume set.

## Step 5 — clean up `session_memory`

When the fold-in completes, drop the resume token to keep
`session_memory` tidy:

```jsonc
mcp__ants__session_memory({
  op: "delete",
  caller_cwd: "<your $PWD>",
  key: "test_audit/<token>/deferred"
})
// Repeat for `partition`, `chunk/…`, `triage/…` as needed.
```

The state is per-cwd-hashed (ANTS-1435), so leaving stale tokens is
harmless beyond mild clutter. The cleanup matters mostly so the next
resume isn't ambiguous about which token to pick up.

## When a thin `test_audit_resume` verb lands

ANTS-1523 leaves the door open for a thin MCP verb that subsumes
Steps 1–3:

- `test_audit_resume(caller_cwd, partition_token: "latest")` would
  resolve the newest token, fetch the deferred list, and run the
  re-verification pass in-engine.
- The verb's response would carry `{surviving:[], dropped:[],
  renamed:[]}` so the orchestrator can branch directly into Step 4.

Until then, this recipe is the documented bootstrap. Sources cited:
MAME_Curator cross-session report 2026-05-18 §4 (the original ask),
ANTS-1283 (session_memory), ANTS-1435 (cwd-hashing), ANTS-1295 (path
validation), ANTS-1457 (false-positive ledger — for findings already
ruled out across sessions).
