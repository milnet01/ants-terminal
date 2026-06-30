# roadmap_log op:"flip" — GFM headline locator (ANTS-3378)

## Purpose
`roadmap_log op:"flip" headline:<text>` must locate a GFM-task-list
bullet by the **canonical headline `roadmap_query` reports**, not only by
the raw post-checkbox head the write-path walker stores. Vestige reported
that flipping by the exact untruncated bold-span of a `- [ ] **ID** — text`
bullet returned `bullet_not_found`: the walker keyed on the raw head
(`**ID** — text`, markdown + bold-ID + em-dash tail intact) while
`roadmap_query` reports the de-marked-up canonical form. On a genuine miss
the suggestions were ranked by shared-prefix over that same raw head, so
they surfaced unrelated bullets.

## Invariants

- **INV-1** — a GFM bullet `- [ ] **ID** — <headline>` flips by
  `headline:"<headline>"` (the post-em-dash prose `roadmap_query` reports),
  returning `{ok:true, op:"flip"}` and rewriting the checkbox to `[x]`.
- **INV-2** — a bold-only GFM bullet `- [ ] **<title>**` (no em-dash)
  flips by `headline:"<title>"` — the `**` emphasis markers are not part
  of the locator token.
- **INV-3** — a plain GFM bullet `- [ ] <text>` (no bold) still flips by
  `headline:"<text>"` (legacy behaviour preserved).
- **INV-4** — a genuine miss returns `bullet_not_found` whose
  `suggestions[]` rank by token overlap against the canonical headline and
  each carries a `line` (1-based) field; an exact-but-different locator is
  not silently matched.

## Test
`tests/features/roadmap_log_flip_gfm_headline/` (label `features;fast`),
driving `RemoteControl::cmdRoadmapLogFlipForTest` against seeded temp
ROADMAPs. INV-1/INV-2 fail against pre-ANTS-3378 source (raw-head match
returned `bullet_not_found`); INV-3 guards the legacy path; INV-4 asserts
the `line` field + relevance ranking.
