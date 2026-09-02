# Feature spec: `section=` covers a heading's descendants on both backends (ANTS-4819)

## Problem

`roadmap_query mode:"section_index"` tallies each section with
`RoadmapIndex::rollupCounts`, which is descendant-aware: a child's counts
bubble into every ancestor. A `section=` read of the same slug did not
agree, and which way it disagreed depended on the backend:

- **Markdown.** `sliceSection` returns `[lineStart, lineEnd)`, and
  `buildIndex` sets `lineEnd` to the next heading of level ≤ its own — so a
  parent's slice spans its children and their bullets are parsed. Agrees
  with the rollup.
- **Store.** The record list was filtered on `sectionSlug == slug`. A
  parent heading owns no records directly, so the read returned nothing
  while the index promised the children's bullets.

Measured on this project's own roadmap: `section_index` reported a large
id-bearing count for a level-2 slug whose `section=` read returned
`count: 0` under every status filter.

## Contract

`RoadmapIndex::descendantSlugs(index, section)` returns the slugs of
`section` and of every section nested inside it. Nesting is
`[lineStart, lineEnd]` containment — the same relation `rollupCounts`
walks — so the two surfaces agree by construction rather than by two
rules that happen to coincide.

The store-backed `section=` filter admits a record whose `sectionSlug` is
any of those slugs.

## Invariants

- **INV-1 self.** The section's own slug is always in the returned set.
- **INV-2 descendants.** A nested section's slug is in its ancestor's set,
  at every depth the index tracks.
- **INV-3 no siblings.** A section that merely follows is not in the set.
- **INV-4 leaf.** A section containing no other returns its own slug alone.
- **INV-5 backend parity.** For any section, the bullets the markdown
  slice yields and the records the store filter admits cover the same set
  of section slugs — the property whose absence was the defect.
