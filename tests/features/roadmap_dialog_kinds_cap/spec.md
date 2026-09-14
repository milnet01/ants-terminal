# Feature: roadmap dialog filters every kind and reads a large live roadmap

## Invariants

**INV-1 — every canonical kind has a filter.** For each value of
`RoadmapParse::canonicalKinds()`, `src/roadmapdialog.cpp` carries a
`roadmap-filter-kind-<kind>` checkbox entry, and the card glyph lookup reads
the same table.

**INV-2 — the live roadmap is read past the per-file cap.**
`RoadmapDialog::loadMarkdown(path, false)` returns text past the first 8 MiB
of a larger live file; the live file shares the 64 MiB assembled cap.

## Rationale

The ANTS-5088 performance pass found the kind filter listed twelve of the
enum's kinds, so the others could not be filtered and any kind filter hid
them. `loadMarkdown` cut the live `ROADMAP.md` at 8 MiB with no notice.

## Test surface

`test_roadmap_dialog_kinds_cap.cpp`: INV-1 reads `src/roadmapdialog.cpp`
(located from the test's own path) for each canonical kind; INV-2 writes a
file just over 8 MiB with a marker at its end and calls `loadMarkdown`.

## Regression history

- **ANTS-5088:** the two defects above. Locked by this spec.
