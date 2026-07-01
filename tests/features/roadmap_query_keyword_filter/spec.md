# roadmap_query `query` keyword filter — Feature Spec (ANTS-3391)

## Purpose
`roadmap_query` previously parsed a `query` arg but discarded it (surfaced
only as a non-fatal `ignored_args:["query"]` advisory), so a caller could
not search roadmap items by keyword — they had to fetch everything and
filter by hand. This adds a case-insensitive substring filter over each
bullet's text, composing with the existing `status` + `section=` filters.
It is a list-path filter, not a targeted selector.

## Invariants

- **INV-1** — `mcp::bulletMatchesQuery(bullet, needle)` is a
  case-insensitive substring test over the bullet's `headline` field:
  `"Crash"` matches a `"Crash on resume"` headline and `"crash"` matches it
  too (both directions case-folded).
- **INV-2** — the match also covers the `body` field, so a keyword that
  appears only in the continuation prose (not the headline) still matches.
- **INV-3** — the match also covers `headline_full` (the untruncated text
  emitted when a long headline was capped at 120 chars), so a keyword past
  the cap is still findable.
- **INV-4** — a needle absent from all three text surfaces returns false.
- **INV-5** — `cmdRoadmapQuery` wires the filter on BOTH emission branches
  via `applyQueryFilter` / `mcp::bulletMatchesQuery`, refuses to combine
  `query` with the targeted / aggregate surfaces (`bad_mode_combo` for
  `id` / `ids` / `mode:section_index` / `mode:bundles`), and echoes the
  applied `query` back in the envelope.
- **INV-6** — `roadmap_query`'s inputSchema declares the `query` property
  (so the dispatch layer recognises it and no longer flags it in
  `ignored_args`).

## Test
`tests/features/roadmap_query_keyword_filter/` (label `features`), in the
`test_core` bundle. INV-1..4 drive the pure `mcp::bulletMatchesQuery`
(Qt6::Core, in `ants_core_lib`); INV-5/INV-6 source-scrape
`remotecontrol.cpp` (`SRC_RC_CPP`) and `claudeintegration.cpp`
(`SRC_CLAUDE_INTEGRATION_CPP_PATH`) for the wiring. Verify INV-1..4 fail
against pre-ANTS-3391 source (the matcher did not exist).
