# roadmap_round_trip — the round-trip oracle and relationship acyclicity

Feature contract for **ANTS-3810** INV-1 to INV-4.
Parent spec: [`docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md`](../../../docs/specs/ANTS-3810-round-trip-oracle-and-acyclicity.md)

Not `roadmap_export_roundtrip/`, which is export → rebuild → re-export
(ANTS-3761's INV-1). This directory is render → load → export (ANTS-3758's
INV-1), plus the whole-store cycle check.

## What this locks

**INV-1 — the full round trip loses nothing and invents nothing.** A source
store is rendered into a scratch root, rediscovered with `findRoadmaps()`,
planned, loaded into a second store, and both stores are exported. The two
exports, projected the same way, must be equal line for line, in order. The
projection drops family 1 (internal and dropped items, with their elements and
the `rel` lines naming them), family 2 (`history`, `citation`, `feedback_ref`,
`id_prefix`) and family 3's item keys, including `extras.source_kind` /
`source_status`. `rel` lines are compared.

**INV-2 — a cycle is reported, not refused.** `A → B` then `B → A` under
`blocked-by`: both writes succeed, the report holds one cycle with its smallest
`(export_slug, id_fold)` first, no cycle is reported before the closing edge,
and an unopened store yields `nullopt` with an error, never a clean report.

**INV-3 — the oracle discriminates.** All six pipeline stages are asserted
before the comparison; the projection holds `item`, `section`, `element`,
`legend` and `rel` records; each projected item carries every markdown field;
family 1 and family 3 are absent; an archive section keeps its `source`; and a
reordered sequence compares unequal.

**INV-4 — whole store, four types, one at a time.** A three-project
cross-project cycle is found; `blocked-by` and `duplicate-of` in opposite
directions, and a `relates-to` triangle, report nothing; both unresolved
shapes count one each; two back edges report two cycles; and more than
`kMaxCyclesPerType` back edges of one type stop at the cap with `truncated`.

## Fixture notes the build proved

- The source store holds what a migrated store holds: each file's head intro
  carries the render's generated-file notice, and each archive file has its
  own synthetic root section.
- The legend carries a `dropped` wording: the render writes that line whatever
  the store holds (ANTS-4977).
- The hidden items are filed last in their section. Their positions are not
  carried by markdown, so an element after them would renumber on re-load.

## Red proofs

Each case was shown red against one mutation, then restored:
INV-1 — the migration storing the whole bullet in `item.body` (pre-ANTS-3808);
INV-2 — the walk recording no cycle; INV-3 — the projection dropping every
line; INV-4 — the walk keeping only same-project edges.
