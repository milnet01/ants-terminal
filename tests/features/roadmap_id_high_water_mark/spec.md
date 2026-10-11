# ANTS-5638 — the header id high-water mark, and the rebuild that reads it

The governing text is `docs/standards/roadmap-format.md` § 3.5.1, from
"On a machine with no store" to "ANTS-5638's tests check this form". This
file names what the tests assert; the standard owns the rules.

## Problem

The store lives outside the repo. Another machine can open a store-migrated
project with no store row, and then allocates from the files' ids alone. An
id the files no longer show, such as an `internal` item's, can be issued
twice.

## Invariants

- **INV-1 — the line's form.** `formatIdHighWaterMark` writes
  `<!-- ants-roadmap-id-high-water: K=N ... -->`, pairs sorted by key in byte
  order, one space apart. No pair, no line. `findIdHighWaterMark` reads only
  the line directly after the generated-file notice. A line there that opens
  with the tag and does not parse is `present`, not `valid`, and has no pairs.
- **INV-2 — the store's pairs.** `RoadmapStore::idHighWaterMark` gives every
  prefix from an `id_prefix` row key or a stored id its `allocationFloor`.
  It gives `P#S` the greater of the `P#S` row and `maxSynthesisedId(P)`
  where either exists. A prefix the grammar refuses gets no pair.
- **INV-3 — the render writes it.** An ants-v1 render puts the mark on the
  line after the notice in the live roadmap. Two renders of one store write
  the same bytes.
- **INV-4 — the import carries it.** Migrating a rendered file records the
  mark on `MigrationPlan::idMark` and keeps it out of the root intro. The
  INV-11 partition still holds: `roadmap_migrate_read`'s
  `Ants5638IdMarkIsItsOwnSpan`. A re-migration and re-render writes one mark
  line, not two.
- **INV-5 — the load raises it.** With `raiseIdMark`, the load raises each
  pair's row before it allocates, so a synthesised id floors on the mark.
  Without it the rows are untouched. A refused load leaves no row.
- **INV-6 — the rebuild.** `op:"append"` and `op:"append_batch"` on a
  project with no store row, whose roadmap carries the notice and classifies
  ants-v1, rebuild the store first and allocate above the mark. The reply
  carries `store_rebuilt`. Its `id_mark` is `"applied"`, or
  `"ignored_unparseable"` for a broken mark. A dry run rebuilds nothing.

## Test

`test_roadmap_id_high_water_mark.cpp`, suite `RoadmapIdHighWaterMark`, in the
`test_claude` bundle. Each case's name opens with the INV it checks.
