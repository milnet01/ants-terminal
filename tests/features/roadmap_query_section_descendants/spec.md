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
- **INV-6 own slug** (ANTS-4824). Each returned bullet's `section_slug` is
  the slug of the section it lives in, not the one that was queried.

  ANTS-1287-INV-7 overwrote the field with the requested slug. That was
  correct while `section=` returned a section's own bullets: the two named
  one value, and the overwrite defended only against a slice-local slugger
  artifact, since a slice cannot reproduce mid-document `-N` suffixes.
  Descendant inclusion made them diverge, so every child bullet reported
  its parent — and the value stopped being safe to feed back to
  `roadmap_log op:"append"`, which files by slug and would land in the
  parent without saying so.

  The set this feature already computes is what separates the two cases: a
  slug the index placed in the subtree is the bullet's own, and anything
  else is the artifact INV-7 named and still falls back to the requested
  slug. INV-6 drives the real `cmdRoadmapQuery` envelope rather than
  re-deriving that rule, and asserts the descendant inclusion still holds
  first, so it cannot pass by returning nothing.
