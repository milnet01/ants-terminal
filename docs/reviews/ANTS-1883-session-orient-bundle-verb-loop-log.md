# ANTS-1883 — `session_orient` bundle verb — review loop log

Record for [`../specs/ANTS-1883-session-orient-bundle-verb.md`](../specs/ANTS-1883-session-orient-bundle-verb.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-26).** Spec drafted as a composer over three
  existing ETag-supported verbs. Verified against
  `cmdSessionBrief` (ANTS-1724 precedent), `cmdCurrentState`
  (ANTS-1569), `cmdProjectLayout`, `cmdRoadmapQuery`'s
  section_index mode (ANTS-1437). All upstream verbs are in
  `isEtagSupportedTool` today; bundling their payloads into one
  envelope and re-hashing through `applyEtagPattern` is the
  established cache pattern. No code changes required upstream.
- **Loop 2 (2026-05-26).** Reviewer flagged 3 HIGH (claimed
  status:"active" is a no-op for section_index — DISMISSED:
  ANTS-1437 INV-7 was amended by ANTS-1848 to make
  status:"active" meaningful for section_index — drops
  sections with active_count_id_only:0; INV-6 vs §2.1 split
  the partial-failure contract across two sections; missing
  token_cost bucket), 2 MEDIUM (Required-contract row hand-wavy
  in §2.2; cache-flip trade-off undocumented), 4 LOW/INFO
  (cosmetic). Verified ANTS-1437 amendment at
  `docs/specs/ANTS-1437-roadmap-query-section-index-mode.md:188-204`; `tokenCostFor` precedent at
  `claudeintegration.cpp:6343-6347`. Fixes: dismiss H1 (annotated
  INV-2 with the ANTS-1848 amendment cite); consolidate §2.1
  partial-failure prose into one paragraph; add INV-7 + §2.2 row
  for `tokenCostFor` registration; name all four
  `claudeintegration.cpp` touchpoints explicitly in §2.2;
  document the bundle-ETag-flips-more-often trade-off in §2.3.
