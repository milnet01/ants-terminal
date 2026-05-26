# `roadmap_query` `mode:"headline_only"` — feature-conformance test

Locks the invariants in `docs/specs/ANTS-1881.md` against the
implementation in `src/remotecontrol.cpp` + `src/claudeintegration.cpp`.

## Invariants tested

| INV | Anchor in `test_roadmap_query_headline_only.cpp` | What it checks |
|-----|---------------------------------------------------|----------------|
| INV-1 | `Inv1ModeAcceptedAndRejected` | `cmdRoadmapQuery` allow-set contains the literal `"headline_only"` alongside `"bullets"` and `"section_index"`. |
| INV-2 | `Inv2KeySetExactlyFour`, `Inv2RollupEmpty`, `Inv2NarratorHeadlineNonEmpty`, `Inv2FieldsDoesNotNarrowBullets` | Projection helper emits only `id`/`status`/`headline_oneline`/`section_slug`; rollup vs narrator empty-value contract. |
| INV-3 | `Inv3CombinatorIdParity` | Headline-only emission walks the same `m_roadmapCacheBullets` iteration as bullets-mode (positional `id` parity). |
| INV-4 | `Inv4SameModeShortCircuit`, `Inv4CrossModeNoShortCircuit` | ETag computed over projected payload (existing `applyEtagPattern` wrapper at dispatcher fires for `roadmap_query` per `isEtagSupportedTool`). |
| INV-5 | `Inv5CombinatorCoverageSourceGrep`, `Inv5IdSelectorProjected` | Projection applies at BOTH emission surfaces (main loop AND id-branch). |
| INV-6 | `Inv6PaginationOnProjectedSet` | `PaginationEngine::pageBullets` is called on the projected array, not the unprojected. |
| INV-7 | `Inv7ToolsListEnumerates` | `modeEnum.append("headline_only")` exists in the `roadmap_query` `tools/list` builder. |
| INV-8 | `Inv8DuplicateIdsParity` | `duplicate_ids[]` emission gate is independent of mode (shared `m_roadmapCacheDuplicateIds` cache, all bullets-emission branches surface it). |

## Test style

Source-grep against `src/remotecontrol.cpp` + `src/claudeintegration.cpp`
via the `SRC_REMOTECONTROL_CPP_PATH` / `SRC_CLAUDEINTEGRATION_CPP_PATH`
compile-time anchors (same pattern as
`tests/features/roadmap_query_section_index/`). The pure projection
helper introduced by the implementation is exercised directly via its
header.

## Pre-fix verification

Before the implementation lands, every test in this file is expected
to FAIL — the source files don't yet contain the `"headline_only"`
literal or the `mode_headline_only` projection anchor. The fix is
restored, the tests turn GREEN.
