# Writes on a store-served pass-headings roadmap go through the store (ANTS-5334)

## Context

ANTS-4803 made the store serve a `#### Pass N.M` roadmap for reads and the
render. Writes still took the markdown path: the pass-headings gate at the top
of `cmdRoadmapLogFlip` and `cmdRoadmapLogAppend` handed the call to the pass
file writer before any store check. So the first edit wrote the file behind
the store, and the next render discarded it. RetroDB reported it; RetroDB
waits on it to stay migrated.

## Contract

On a pass-headings project the store serves:

- `op:"flip"` and `op:"annotate"` write the item through the store's write
  sequence (`rlStoreFlipOrAnnotate`, shared with ants-v1), and the render
  publishes the file. The pass is located by the pass writer's own rule: its
  `PASS-N-M` id or its heading tail.
- `op:"append"`, `op:"append_batch"`, `op:"flip_batch"` and
  `op:"annotate_batch"` have no store route yet. They refuse
  `unsupported_format` and write nothing.

A pass-headings project the store does not serve keeps every file writer.

## Invariants

- **INV-1** — `op:"flip"` changes the item's status in the store, and the
  rendered file carries the pass's new Status keyword. The envelope says
  `format:"pass-headings"`.
  *Test:* `Inv1FlipWritesThroughTheStore`.
- **INV-2** — a `dry_run` flip names the file it would write
  (`would_write`) and changes neither the store nor the file.
  *Test:* `Inv2DryRunPreviewsWithoutWriting`.
- **INV-3** — `op:"annotate"` appends its note to the item's stored body and
  leaves its status alone.
  *Test:* `Inv3AnnotateAppendsToTheStoredBody`.
- **INV-4** — each of the four ops with no store route refuses
  `unsupported_format`, and the file and the store's items are unchanged.
  So does `op:"amend_body"`. Every such refusal names `roadmap_migrate` as the
  route that works: edit the file by hand, then re-import it (ANTS-5396).
  *Test:* `Inv4OpsWithNoStoreRouteRefuse`.
- **INV-5** — on a pass-headings project the store does not serve,
  `op:"flip"` still writes the file directly. A boundary pin: it holds before
  and after the fix.
  *Test:* `Inv5UnmigratedPassProjectStillWritesTheFile`.
- **INV-6** — a store-route flip that changes status replaces the date
  written right after the status word on the item's first Status line with
  today's, keeping the rest of the line (`Lanes:` included). A note goes
  above a trailing `---` / `***` / `___` rule, inside the item. The file
  path's annotate places a note by the same rule (ANTS-5395).
  *Tests:* `Ants5395FlipRedatesAndKeepsTheNoteInside`,
  `Ants5395FileAnnotateKeepsTheNoteInside`.
- **INV-7** — a note is written as a bullet, never a bare line, which
  Markdown would fold into the bullet above it (ANTS-5404). A store-route flip
  to shipped writes `- **Resolution** (<today>): <note>` directly above the
  first Status line; any other note is `- **Progress** (<today>): <note>` at
  the INV-6 position. A note that already opens with `- ` is kept as written.
  A store-route flip that changes status writes the word this roadmap already
  uses for the target status, by majority over its items' Status lines, and
  the canonical keyword only when no item uses one. Fixture shapes are
  RetroDB's (`roadmap.md` at 075f494, Passes 59.64, 59.71, 59.72).
  *Tests:* `Ants5404ShippingNoteIsAResolutionAboveStatus`,
  `Ants5404AnnotateNoteIsAProgressBullet`,
  `Ants5404FileAnnotateNoteIsABullet`.

INV-1 to INV-4 fail against the pre-fix code.

## Build

Compiled into the same bundle as `roadmap_write_half`, whose store fixture it
follows. Label `features;fast`.
