# Feature spec: `indie_review_partition` sparse-partition hint (ANTS-3567)

## Problem

`cold_eyes_partition` emits `sparse_partition` + `sparse_partition_hint` +
`next_step_hint` (ANTS-1634a / ANTS-1571) when a default-scope partition
comes back near-empty, pointing callers at the `cold_eyes_brief`
`doc_paths[]` ad-hoc escape hatch and the `.cold-eyes/partition.json`
override.

`indie_review_partition` (the module-map-driven code-review analogue) had
**no** equivalent: when a project lacks a CLAUDE.md `## Module map` /
`docs/subsystems.md`, the derived partition is empty and the caller gets no
pointer to the now-available `indie_review_brief` `source_paths[]` ad-hoc
mode (ANTS-3375). The observed failure pattern is "give up and skip the
verb" (cross-session reports). This is a symmetry follow-up to ANTS-3375.

## Surface

`RemoteControl::cmdIndieReviewPartition` (the remotecontrol TUs).

## Invariants

- **INV-1 — hint names the ad-hoc escape hatch.** When the partition is
  sparse, `sparse_partition_hint` points at
  `indie_review_brief(lane=…, source_paths=[…])` and cites ANTS-3375 (the
  code-review analogue of `cold_eyes_brief` `doc_paths[]`).
- **INV-2 — hint names the override file.** The hint also points at
  `<projectPath>/.indie-review/partition.json` as the persisted-override
  path.
- **INV-3 — gated on a sparse partition.** The `sparse_partition` boolean
  and the hint are emitted only when the deriver yields ≤1 lane, mirroring
  `cmdColdEyesPartition`'s `lanes.size() <= 1` gate — never on a healthy
  multi-lane partition.

## Test scope

Source-scrape against the remotecontrol TUs, scoped to the
`cmdIndieReviewPartition` function body (the wrapper needs a MainWindow, so
a runtime drive is out of scope — this mirrors how the sibling
`mcp_cold_eyes` sparse-hint test greps `cmdColdEyesPartition`). The
existing `mcp_cold_eyes` test is updated in the same change to scope its
lookup to `cmdColdEyesPartition` (a second `sparse_partition_hint` now
precedes it in file order).
