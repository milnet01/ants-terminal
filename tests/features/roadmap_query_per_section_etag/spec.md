# `roadmap_query` per-section ETag — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1882.md`. The section-mode
`duplicate_ids` field is now filtered to entries involving the
queried section — preserving per-section ETag invariance under
unrelated cross-section edits.

## Anchors

| INV | Test                                           | What it checks |
|-----|------------------------------------------------|----------------|
| INV-1 | `Inv1FilterContract`                         | Pure-function: `rcFilterDuplicateIdsForSection` returns only entries whose `occurrences[]` includes the section slug. |
| INV-2 | `Inv2SectionPathFilters`                     | `cmdRoadmapQuery`'s section branch calls the filter (not the bare cache field). |
| INV-3 | `Inv3SectionResponseInvariantUnderUnrelatedEdit` | Source-grep guard on the filter call site + non-empty gate. |
| INV-4 | `Inv4FullFilePathUnchanged`                  | The full-file path and id-branch still emit the full `m_roadmapCacheDuplicateIds` (cross-section view preserved). |

## Pre-fix verification

Before the fix, every emission path called
`out["duplicate_ids"] = m_roadmapCacheDuplicateIds` directly. The
section-path-specific call to `rcFilterDuplicateIdsForSection` is
the new anchor; absent in pre-fix code.
