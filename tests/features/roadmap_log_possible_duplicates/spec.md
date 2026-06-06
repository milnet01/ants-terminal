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

## Test plan

Behavioural against `cmdRoadmapLogAppendForTest` /
`cmdRoadmapLogAppendBatchForTest` over a `QTemporaryDir` project
(ROADMAP.md + .roadmap-counter), mirroring the
`mcp_roadmap_log_append_batch` harness. No real ROADMAP.md.
