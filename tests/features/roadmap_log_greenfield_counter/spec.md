# roadmap_log greenfield `.roadmap-counter` auto-init (ANTS-3397)

When the counter strategy runs on a roadmap that has **no
`.roadmap-counter` AND no existing bullet ids of any kind**, the verb
auto-creates `.roadmap-counter` at `0` and proceeds, instead of
refusing with `counter_missing` (which forced a shell side-step
`echo 0 > .roadmap-counter` that broke the all-MCP workflow).

A roadmap that already carries ids but has lost its counter file is a
genuine desync — those calls keep refusing with `counter_missing` so
the lost high-water mark is surfaced rather than silently re-allocated.

## Invariants

| INV | Test | What it checks |
|-----|------|----------------|
| 1 | `Inv1SingleAppendGreenfieldAutoInit` | `op:append` on an id-less, counter-less roadmap succeeds, allocates `<prefix>-0001`, and creates `.roadmap-counter` at `1`. |
| 2 | `Inv2BatchGreenfieldAutoInit` | `op:append_batch` (2 bullets) on the same greenfield project succeeds, allocates `<prefix>-0001`/`-0002`, counter ends at `2`. |
| 3 | `Inv3ExistingIdsNoCounterStillRefuses` | A roadmap WITH ids but no counter file still refuses (`counter_missing`) on both `append` and `append_batch` — the real-desync guard. |
| 4 | `Inv4HelperPresent` | `remotecontrol.cpp` carries the `rlRoadmapHasAnyBulletId` greenfield discriminator. |

## Test entry point

Behavioural against `cmdRoadmapLogAppendForTest` /
`cmdRoadmapLogAppendBatchForTest` over a `QTemporaryDir` project
(`ROADMAP.md` only — `.roadmap-counter` deliberately absent), mirroring
`mcp_roadmap_log_atomicity` / `mcp_roadmap_log_append_batch`.

## Pre-fix verification

Against pre-ANTS-3397 source, `Inv1`/`Inv2` FAIL — a greenfield append
returned `counter_missing` (`ok:false`). `Inv4` FAILS — the helper
literal is absent. `Inv3` already passed pre-fix (it is a guard that the
desync refusal is preserved). After the fix all four are GREEN.
