# ANTS-2043 — roadmap_log surfaces non-blocking near-duplicate content

## Background

Ants flags exact duplicate **IDs** (`rcComputeDuplicateIds`, canonical
collisions) but not near-duplicate **content** — two bullets that say
nearly the same thing. A caller filing a new bullet had to grep
ROADMAP.md by hand to avoid re-filing an existing item (done manually
for ANTS-2037..2042).

`roadmap_log op:append` / `op:append_batch` now run an append-time soft
check, reusing the existing `rcNormaliseHeadline` + `rcFnv1a64`
machinery: an exact normalised-headline match scores 100; otherwise a
token Jaccard overlap (shared tokens / union) gates at 60 % with a
≥2-shared-token floor. The result is **advisory** — the bullet is still
appended; the success envelope carries a `possible_duplicates` list.

## Invariants

### INV-1 — exact normalised match is surfaced at score 100

`op:append` of a headline whose normalised form equals an existing
bullet's headline still appends (`ok:true`), and the envelope carries
`possible_duplicates:[{id, headline, score}]` with the existing bullet's
id and `score == 100`.

### INV-2 — near match (Jaccard ≥ 0.6) is surfaced

`op:append` of a headline sharing ≥ 60 % of its tokens with an existing
bullet surfaces that bullet in `possible_duplicates` with a score in
[60, 99].

### INV-3 — a clean headline omits the field entirely

`op:append` of a headline with no exact or near match carries **no**
`possible_duplicates` key (absent, not an empty array).

### INV-4 — the advisory never blocks the append

In every INV-1/INV-2 case the bullet is written to ROADMAP.md and the
counter advances — `possible_duplicates` is a heads-up, not a refusal.

### INV-5 — append_batch attaches the advisory per accepted bullet

`op:append_batch` surfaces `possible_duplicates:[{bullet_index, id,
candidates:[...]}]` — one entry per accepted bullet that has at least
one candidate; clean bullets are omitted and the whole batch still
applies.

### INV-6 — on a migrated project the advisory's records come from the store

`op:append` against a project the roadmap store serves surfaces a store item
whose headline matches, at `score == 100`. ANTS-4426: the advisory used to
parse the whole of `ROADMAP.md` here — 3.8 MiB on this project — which was the
last consumer keeping ANTS-3863's bounded dispatch away from `op:append`. INV-1
to INV-5 all run against an **unmigrated** project and never reached this path.

### INV-7 — the two backends cannot disagree on a successful append

A migrated project whose `ROADMAP.md` carries a bullet the store has never
imported is exactly the input on which a file-sourced advisory and a
store-sourced one differ. On that input `commitAndRender()` refuses before any
envelope is built, so no advisory is produced from either backend. This is what
makes INV-6's swap invisible rather than merely cheaper, and it is the premise
ANTS-2043's own comment asserted ("the file is the render's own output") with
nothing checking it.

## Test plan

Behavioural against `cmdRoadmapLogAppendForTest` /
`cmdRoadmapLogAppendBatchForTest` over a `QTemporaryDir` project
(ROADMAP.md + .roadmap-counter), mirroring the
`mcp_roadmap_log_append_batch` harness. No real ROADMAP.md.

INV-6 and INV-7 add a **migrated** fixture: an ants-v1 `ROADMAP.md` run through
`RoadmapMigrate::findRoadmaps` -> `planFrom` -> `RoadmapMigrateLoad::load` into
a store under an `ants_test::XdgGuard`-redirected `XDG_DATA_HOME`. The ants-v1
marker is load-bearing — `RoadmapSource::migratedProject()` serves the store for
that dialect only, so a fixture without it would carry a store row and still
exercise the markdown branch.
