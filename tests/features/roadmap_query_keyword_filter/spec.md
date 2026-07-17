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
- **INV-8** (ANTS-3560) — when a `query` matches zero bullets on a roadmap
  that still contains id-bearing bullets, the empty-result warning is
  query-specific (`no bullet matched query "<q>"`, naming the count of
  id-bearing bullets searched and hinting at `ids:[...]` for a
  space/comma-separated multi-id query), NOT the ANTS-1538 "every entry
  has no [PROJ-NNNN] id" text. The gate distinguishes the two by capturing
  `postIdPruneCountFull` (the id-bearing count) after the ID-prune but
  before `applyQueryFilter`. Reported by Rolodex (2026-07-16): a
  space-separated multi-id `query` hit the misleading ANTS-1538 warning.
- **INV-7** (ANTS-3420 → ANTS-3422) — the `mainwindow.cpp` `roadmap_query`
  provider forwards `query` (and the ANTS-3402 `max_body_bytes` /
  ANTS-1907 `include_section_etags` / `section_etag_match` companions) to
  `cmdRoadmapQuery`. INV-5/INV-6 proved the handler and schema were
  correct, but the hand-maintained forward list omitted these args, so
  each was dropped at the MCP boundary and inert end-to-end. ANTS-3422
  retired that allowlist for a verbatim
  `rcDelegate(&RemoteControl::cmdRoadmapQuery)` forward that passes the
  whole args object through — so every arg (present and future) reaches
  the handler by construction and the drop bug-class cannot recur.

## Test
`tests/features/roadmap_query_keyword_filter/` (label `features`), in the
`test_core` bundle. INV-1..4 drive the pure `mcp::bulletMatchesQuery`
(Qt6::Core, in `ants_core_lib`); INV-5/INV-6 source-scrape
`remotecontrol.cpp` (`SRC_RC_CPP`) and `claudeintegration.cpp`
(`SRC_CLAUDE_INTEGRATION_CPP_PATH`) for the wiring; INV-7 source-scrapes
`mainwindow.cpp` (`SRC_MAINWINDOW_CPP`) for the dispatch forward. Verify
INV-1..4 fail against pre-ANTS-3391 source (the matcher did not exist) and
INV-7 fails against pre-ANTS-3422 source (the verbatim `rcDelegate`
forward was absent — the provider still used the hand-maintained per-arg
lambda).
