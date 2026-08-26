# roadmap_log greenfield `.roadmap-counter` auto-init (ANTS-3397)

When the counter strategy runs on a roadmap that has **no
`.roadmap-counter` AND no existing bullet ids of any kind**, the verb
auto-creates `.roadmap-counter` at `0` and proceeds, instead of
refusing with `counter_missing` (which forced a shell side-step
`echo 0 > .roadmap-counter` that broke the all-MCP workflow).

**ANTS-3450** — `.roadmap-counter` is now a derived, gitignored cache, so
an absent counter is the normal fresh-clone state, not a desync. When the
roadmap already carries **counter-style ids**, the verb RECOVERS the
high-water mark from the committed corpus (ROADMAP + CHANGELOG +
`docs/roadmap/*.md`, via `RoadmapFoldIn::corpusHighWater`) and allocates
above it, rather than refusing. Only an id-bearing roadmap from which **no
counter-style high-water mark can be recovered** (`corpusHighWater` → 0
while bullets carry ids — e.g. a stable-string-id project) still refuses
with `counter_missing`, the `rlRoadmapHasAnyBulletId` discriminator.

## Invariants

| INV | Test | What it checks |
|-----|------|----------------|
| 1 | `Inv1SingleAppendGreenfieldAutoInit` | `op:append` on an id-less, counter-less roadmap succeeds, allocates `<prefix>-0001`, and creates `.roadmap-counter` at `1`. |
| 2 | `Inv2BatchGreenfieldAutoInit` | `op:append_batch` (2 bullets) on the same greenfield project succeeds, allocates `<prefix>-0001`/`-0002`, counter ends at `2`. |
| 3 | `Inv3ExistingIdsNoCounterRecovers` | A roadmap WITH counter-style ids (`[ANTS-9001]`) but no counter file RECOVERS: `op:append` succeeds, allocates `ANTS-9002` (above the recovered mark, never reissuing 9001), and seeds `.roadmap-counter` to `9002`. |
| 3b | `Inv3bExistingIdsNoCounterRecoversBatch` | Same recovery on `op:append_batch` — 2 bullets consume `9002`/`9003`, counter ends at `9003`. |
| 4 | `Inv4HelperPresent` | `remotecontrol.cpp` still carries the `rlRoadmapHasAnyBulletId` discriminator (now guarding only the unrecoverable-desync refusal). |
| 5 | `Inv5DryRunLeavesNoCounter` | A SUCCEEDING `dry_run` append on the greenfield project creates no `.roadmap-counter` and still reports `would_be_id` `CL-0001` — the same id INV-1's real append allocates. |
| 6 | `Inv6RefusedDryRunLeavesNoCounter` | A `dry_run` append REFUSED with `section_has_subsections` creates no `.roadmap-counter`. The seed runs before the section gate, so this is the case that leaked. |
| 7 | `Inv7BatchDryRunLeavesNoCounter` | A `dry_run` `op:append_batch` creates no `.roadmap-counter` — the batch path carries its own copy of the auto-init. |

**ANTS-4691** — the auto-init above is a WRITE, and it ran on the `dry_run`
path too, before the section and target gates. So a preview that refused and
wrote no bullet still left an untracked `.roadmap-counter` behind. `dry_run`
is documented as writing nothing, and its value is being safe to point at a
repository you do not own; a refused call that touches disk removes that. The
seed value is known without writing it, so a dry run now uses it in memory and
reports the same `would_be_id` the real allocate would hand out.

## Test entry point

Behavioural against `cmdRoadmapLogAppendForTest` /
`cmdRoadmapLogAppendBatchForTest` over a `QTemporaryDir` project
(`ROADMAP.md` only — `.roadmap-counter` deliberately absent), mirroring
`mcp_roadmap_log_atomicity` / `mcp_roadmap_log_append_batch`.

## Pre-fix verification

Against pre-ANTS-3397 source, `Inv1`/`Inv2` FAIL — a greenfield append
returned `counter_missing` (`ok:false`). `Inv4` FAILS — the helper literal
is absent. Against pre-ANTS-3450 source, `Inv3`/`Inv3b` FAIL — an
id-bearing counter-less roadmap returned `counter_missing` instead of
recovering. After both fixes all five are GREEN.
