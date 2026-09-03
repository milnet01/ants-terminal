# Serving the pass-headings dialect from the store (ANTS-4803)

## Context

`RoadmapSource::migratedProject` ended `if (format != "ants-v1") return
nullopt`, so for a `pass-headings` roadmap the store was **write-only**: the
migration filled it, no read consulted it, and every store-side benefit — source
of truth, `op:"render"`, drift detection, cross-project reporting — was
unavailable.

The data side already worked. The migration reads the dialect, folds each
heading to a `PASS-N-M` id and stores sections, items and bodies correctly. What
was missing was the **render**: `passheadingwrite.cpp` edits an existing markdown
file in place, and publishing from the store needs the inverse — emitting a whole
document from rows.

Converting the reporting project was ruled out: it measured roughly 1,800
`Pass N.M` citations across ~230 files, including three test module filenames.
It de-registered from the store twice rather than leave stale rows in the
machine-global figures.

## Contract

`RoadmapRender::Options::dialect` selects the emission. Empty or `"ants-v1"`
emits bullets; `"pass-headings"` emits `#### Pass N.M` blocks.

**Only the per-item emission differs.** Sections, ordering, narration, tables,
the file split and the drift report are the same machinery, because the store's
shape is the same and only the surface spelling is not. A second `render()`
would have been a second place for all of that to drift.

Callers pass the project's **stored** `source_format`, so a project cannot be
published in a dialect it was not migrated from.

Two consequences worth stating outright:

- **The designator is recovered from the id**, not stored. `passDesignator`
  exists on the parse record and in no store column, and adding one would be a
  `kSchemaVersion` bump — a one-way door across every project on the machine.
  Deriving `43.5` from `PASS-43-5` needs no migration.

- **The Layman gate does not apply to this dialect.** A `#### Pass` block has
  nowhere to put a Layman line, so an open pass item can never satisfy that
  gate; applying it would refuse every publish forever. Measured: the first
  round-trip run refused two of three items for exactly this. The waiver is
  scoped to the emission format, not waived per item, because the reason is the
  format's — no author has failed to do something they could have done.

## Invariants

INV-1 and INV-2 are the new-behaviour pins and go red against the pre-fix code.
INV-3 and INV-4 are boundary pins and hold in both states.

- **INV-1** — migrate, render, then migrate that output and render again:
  byte-identical. Byte-stability across the second trip is what proves the
  render is the migration's inverse. Comparing against the author's own seed
  would be the wrong bar — a render canonicalises, exactly as the ants-v1 one
  does.
  *Test:* `Inv1MigrateThenRenderIsByteStable`. **Fails against the pre-fix
  renderer**, which emits bullets for every dialect.

- **INV-2** — the read seam serves a migrated pass-headings project from the
  store. This is the gate the item is named for.
  *Test:* `Inv2ReadSeamServesPassHeadingsFromTheStore`. **Fails against the
  pre-fix gate.**

- **INV-3** — the designator survives, sub-pass included, and an id of another
  shape yields nothing rather than a wrong designator.
  *Test:* `Inv3DesignatorRecoveryIsTheExactInverse`.

- **INV-4** — an ants-v1 project still renders as bullets with the dialect
  unset. This is the regression a shared render path could cause.
  *Test:* `Inv4AntsV1StillRendersAsBullets`.

The harness deletes the seed file before rendering. Without that, a render that
never ran leaves the author's own bytes on disk and every assertion passes for
the wrong reason.

## Deliberately not covered

`github-task-list`. It remains markdown-served: nothing renders it back, and
this change deliberately did not widen the gate beyond what the render can
publish. `roadmap_read_seam` INV-1 pins that, and its fixture moved to that
dialect because pass-headings is no longer an example of a foreign one.
