# roadmap_query_filter_section_headers — ANTS-1398

See `docs/specs/ANTS-1398.md`.

## Test scope

Source-scrape regression locks the rollup-filter behaviour of
`cmdRoadmapQuery` and the `include_section_headers` opt-in flag.
Matches the source-scrape style of the sibling
`mcp_roadmap_status_filter` test for ANTS-1247.

## Invariants checked

- **INV-1.** `cmdRoadmapQuery` reads `include_section_headers`
  from `req` (default false).
- **INV-2.** A rollup predicate (matched by anchor comment
  `ANTS-1398-INV-2`) appears in `remotecontrol.cpp` and tests
  emptiness of `id` AND `headline`.
- **INV-3a.** Full-file emission path applies the filter post-
  status filter when `include_section_headers` is false.
- **INV-3b.** Section-mode emission path applies the filter
  post-status filter too.
- **INV-4.** The `include_section_headers` schema property is
  declared on the `roadmap_query` tool descriptor in
  `claudeintegration.cpp`.
- **INV-5.** The opt-in echo field `include_section_headers`
  is emitted on the envelope only when the request set it
  (anchor comment `ANTS-1398-INV-5`).
