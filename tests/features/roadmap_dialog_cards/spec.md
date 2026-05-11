# roadmap_dialog_cards — ANTS-1154

Card-renderer contract for the v2 RoadmapDialog. Verifies:
- `parseBullets` exposes the three new fields (`layman`, `body`,
  `sectionSlug`) the cards need.
- `renderCardsHtml` emits the load-bearing HTML shape — one card
  per top-level status-emoji bullet, with state icon, kind chip,
  summary, expand toggle, and a meta row.
- Tab-relevance gating: non-Full presets strip prose narration
  and suppress empty section headers.
- `parseShippedDates` maps each `[ANTS-NNNN]` in CHANGELOG.md
  release-block bodies to the section date.

The pure helpers stay testable without instantiating the dialog.

## Invariants

INV labels qualified `ANTS-1154-INV-N`. All citations are against
`src/roadmapdialog.{cpp,h}` post-implementation.

| #  | Lane     | Statement |
|----|----------|-----------|
| 1  | renderer | `renderCardsHtml` emits each top-level status-emoji bullet wrapped in `<div class="rm-card" id="rm-ANTS-NNNN">`. Anchor: `// ANTS-1154-INV-1` on the line preceding the literal. |
| 2  | renderer | Each card carries `<span class="rm-state">`, optional `<span class="rm-kind">`, `<span class="rm-summary">`, and an `<a class="rm-toggle" href="ants://expand/<id>">` (or `ants://collapse/<id>` when expanded). |
| 3  | renderer | Summary text is `BulletRecord::layman` when non-empty, else `BulletRecord::headline` with any leading `ANTS-NNNN — ` token stripped. |
| 4  | parser   | `parseBullets` extracts a `Layman:` line into `BulletRecord::layman`. Anchor: `// ANTS-1154-INV-4` preceding the `Layman:` literal in the regex. |
| 5  | parser   | `parseBullets` populates `BulletRecord::sectionSlug` from the most recent `##` / `###` heading. Slug rules: lowercase, non-alnum → `-`, leading/trailing dashes trimmed. |
| 11 | renderer | On every preset except `Full`, prose narration paragraphs (text between heading and first bullet, and any non-status bullets) MUST NOT render in the output HTML. |
| 12 | renderer | On every preset except `Full`, a section header MUST NOT render if its filtered bullet count is zero. |

Plus a cross-cutting parser test:

- **ShippedDates** — `parseShippedDates` walks `CHANGELOG.md`
  release-block headings (`## [X.Y.Z] — YYYY-MM-DD`) and maps every
  `[ANTS-NNNN]` token in the section body to that date. IDs that
  appear in multiple sections keep the first-encountered date
  (i.e. the earliest shipped version, since CHANGELOGs are written
  newest-first by convention — Keep-a-Changelog convention is the
  inverse, so the earliest position is the most-recent release).

## Acceptance

`ctest -R RoadmapDialogCards` exits zero.

The test instantiates no GUI — `renderCardsHtml`, `parseBullets`,
and `parseShippedDates` are public statics. Fixtures are inline
markdown / changelog strings written into the test, not on-disk
files (no QTemporaryDir).

## CMake wiring

The test source goes into the existing `test_dialogs` bundle's
SOURCES list (mirrors `roadmap_kind_facets`'s wiring). No new
`add_executable` per ANTS-1217.

## Memory budget

Zero new fields. The test allocates a single QString of synthetic
markdown (~2 KiB) and one CHANGELOG fixture (~1 KiB). Source-grep
reads the cpp file at compile time via the `ROADMAPDIALOG_CPP`
compile definition.

## Pre-fix verification

To confirm the source-grep tests would catch a real regression:

- **INV-1:** delete the `// ANTS-1154-INV-1` anchor comment in
  `roadmapdialog.cpp`. Source-grep step turns red.
- **INV-4:** delete the `// ANTS-1154-INV-4` anchor comment.
  Source-grep step turns red.
- **INV-11:** remove the `if (opts.activePreset != Preset::Full)
  continue;` guard in the prose-emission branch. INV-11 test fails
  because prose appears in History-preset output.
- **INV-12:** remove the `sectionVisible` gating around the
  `emitSectionHeader` call. Empty sections render their headers,
  failing INV-12.

Restore each before committing.
