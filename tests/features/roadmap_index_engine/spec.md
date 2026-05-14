# ANTS-1287 — roadmap_index_engine feature test

## What it locks

The pure-engine surface of `RoadmapIndex` (`src/roadmapindex.h`):

1. **ENG-1.** `buildIndex` emits exactly one `Section` per `##` / `###`
   heading in document order. H1 and H4 are not indexed (INV-3 — parity
   with `parseBullets`).
2. **ENG-2.** `lineStart` points at the heading line itself (0-indexed).
   `lineEnd` is exclusive: equal to the next-equal-or-shallower
   heading's `lineStart`, or total_lines for the last section.
3. **ENG-3.** Duplicate-heading-text gets walk-order suffixed slugs
   (`performance`, `performance-2`, `performance-3`) — INV-4 parity
   with `parseBullets`.
4. **ENG-4.** `findBySlug` returns the section by slug; nullptr on miss
   and on empty slug.
5. **ENG-5.** `sliceSection` returns the substring covering
   `[lineStart, lineEnd)` joined with '\n'. The first line of the
   slice IS the section's heading.
6. **ENG-6.** `slugifyHeading` collapses non-alnum runs to single
   `-`, lower-cases, trims trailing `-` (byte-identical to the old
   `roadmapdialog.cpp:485` static).
7. **ENG-7.** Nested `### child` inside `## parent` gets `level=3` and
   ends at the next `###` (sibling) or `##` (parent's next sibling).

## Test shape

Pure engine, no QProcess, no GUI, no QTemporaryDir. GTest binary
linking `ants_core_lib` only (Qt6::Core).

Sample markdown is generated inline so the test does not depend on
ROADMAP.md drift.
