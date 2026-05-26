# Feature spec: roadmap_query section_shape hint for non-bullet sections (ANTS-1696)

User pain (2026-05-20): querying `section="bundle-plan-for-pulls-…"`
returned `bullets:[] count:0` even with `include_narrator_bullets:true +
include_body:true` because the section body is a `| … |` planning table,
not roadmap-format bullets. The empty response is indistinguishable from
a genuinely empty section, so the caller falls back to a raw `Read`
(~2-5 K tokens for a multi-row table). ANTS-1696 surfaces a hint so the
caller knows immediately to use `Read` for that section.

## Surface

When a `section=` query returns `count:0` AND the section slice has
non-bullet content lines (table rows or paragraph prose), the envelope
gains:

- `section_shape`: `"table"` | `"prose"` | `"empty"`
- `non_bullet_lines`: integer count of non-blank, non-heading,
  non-bullet content lines in the slice

Hint absent when the section has actual bullets (back-compat) or when
the section is truly empty (no content at all).

## Invariants

- **INV-1 / table detection.** A slice classifies as `"table"` when at
  least one non-bullet content line begins with `|` (after leading
  whitespace strip). Source anchor: `rcSectionShape` helper in
  `remotecontrol.cpp`.
- **INV-2 / prose fallback.** A slice with non-bullet content lines that
  do not start with `|` classifies as `"prose"`. Source anchor: same
  helper, ELSE branch after the `|` test.
- **INV-3 / empty stays empty.** A slice with no non-blank,
  non-heading, non-bullet lines classifies as `"empty"` and the hint
  is omitted from the envelope (the response stays back-compat).
- **INV-4 / hint conditional on count:0.** The `section_shape` /
  `non_bullet_lines` fields appear ONLY when the section-mode emit's
  pre-filter parsed `sectionBullets.isEmpty()` — i.e. parseBullets
  found no bullets in this slice. A section with bullets that the
  status filter happened to drop does NOT carry the hint (the parsed
  set wasn't actually empty).
- **INV-5 / shape cached by slug.** `m_roadmapSectionShape` caches the
  `{shape, non_bullet_lines}` result per slug so subsequent queries
  against the same section don't re-classify. Cleared in lockstep
  with `m_roadmapSectionCache` on the mtime-stale wipe path.
- **INV-6 / bullet/heading lines excluded from the count.** Lines
  beginning with `# `, `## `, `### `, `- `, `* `, `+ `, or `1.` etc.
  do not contribute to `non_bullet_lines`. Blank lines and pure
  whitespace lines also excluded.
- **INV-7 / schema description mentions hint.** The roadmap_query
  tool descriptor calls out the new envelope shape so callers
  reading the catalogue know it exists. Source anchor: `ANTS-1696`
  string in `claudeintegration.cpp`.
