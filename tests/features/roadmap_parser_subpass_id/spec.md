# ANTS-2035 — pass-heading sub-pass IDs are distinct from their parent

## Background

`roadmap_query` reads `#### Pass N.M …` heading roadmaps via
`RoadmapDialog::parsePassHeadingBullets` (ANTS-1530), synthesising
`PASS-<major>-<minor>` IDs. The synthesis regex captured only
`(\d+)\.(\d+)`, so a sub-pass heading `#### Pass 41.5.B` matched
`major=41, minor=5` and the trailing `.B` fell into the headline
tail — producing the **same** ID `PASS-41-5` as its parent
`#### Pass 41.5`. The duplicate-ID detector (`rcComputeDuplicateIds`,
ANTS-1646/1688) keys on the synthesised ID, so it reported the parent
and every `.LETTER` sub-pass as a false collision. RetroDB confirmed
the false set `PASS-47-6 / 41-5 / 41-6 / 41-13` across four sessions
(v3.6.28→34) even though each `#### Pass` heading appears once on disk.

ANTS-1688 (shipped) already fixed a *different* facet — the detector
keying on non-ID tokens (hash nonces / anchors). This facet is at
ID *synthesis* time, upstream of the detector.

## Invariants

### INV-1 — parent and sub-pass get distinct IDs

`parsePassHeadingBullets` on a doc containing `#### Pass 41.5` and
`#### Pass 41.5.B` yields two bullets with **distinct** ids:
`PASS-41-5` for the parent and a sub-pass-suffixed id (`PASS-41-5-B`)
for the sub-pass. Two further sub-passes (`.A`, `.B`) of the same
parent are likewise distinct from each other.

### INV-2 — no false duplicate from sub-passes

`rcComputeDuplicateIds` over the parsed bullets reports **zero**
duplicate ids for a doc whose only repetition is a parent + its
`.LETTER` sub-passes (the RetroDB false-positive shape).

### INV-3 — parent-only behaviour unchanged

A heading with no sub-pass suffix (`#### Pass 7.2 (CRITICAL, S) Fix`)
still synthesises `PASS-7-2` byte-for-byte — the fix is additive and
does not perturb the established numbering.

### INV-4 — numeric third level is not a sub-pass

The sub-pass suffix is letter-led (`.B`, `.Hotfix`). A purely numeric
third level (`#### Pass 3.1.2`) is left in the headline tail as before
(documented `.LETTER` scope only), so the fix is surgical.

## Test plan

Behavioural test against `RoadmapDialog::parseBullets` /
`rcComputeDuplicateIds` with synthetic pass-heading fixtures (no real
ROADMAP.md). `parseBullets` dispatches to `parsePassHeadingBullets`
when the doc is detected as `pass-headings` format (≥2 `#### Pass`
headings + ≥2 `- **Status**:` markers, no ants-v1 emoji). The
duplicate detector is exercised through its public free function.
