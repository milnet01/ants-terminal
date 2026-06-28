# roadmap_scroll_persistence — ANTS-1264

Feature contract for the Roadmap dialog's scroll-position persistence
(implements spec `docs/specs/ANTS-1154.md` §4.5 / INV-13).

The dialog records the topmost visible card per tab on close
(`Config::roadmapScrollAnchors`, keyed by the active preset) and restores
it on the next open. The anchor is ID-keyed — `(sectionSlug, id,
offsetPx)` — so it survives ROADMAP.md edits between sessions rather than
pinning a brittle raw pixel offset.

The pixel capture/restore glue is best-effort GUI code (it never crashes
and degrades to "top" when it cannot place the target). The load-bearing,
deterministic contract is the pure resolver
`RoadmapDialog::resolveScrollAnchor`, which this test drives directly.

## Invariants

- **INV-1 (card hit)** — when the saved `id` is among the rendered card
  ids, the resolver returns `Card` carrying that id and the saved
  `offsetPx`.
- **INV-2 (section fallback)** — when the saved `id` is gone but its
  `sectionSlug` is still among the rendered sections, the resolver returns
  `Section` carrying that slug (the offset is dropped).
- **INV-3 (top fallback)** — when neither the `id` nor the `sectionSlug`
  survives, the resolver returns `Top`.
- **INV-4 (card precedence)** — when both the `id` and the `sectionSlug`
  are present, `Card` wins over `Section`.
- **INV-5 (empty anchor)** — an anchor with empty `id` and empty
  `sectionSlug` resolves to `Top` (empty fields never match a present
  entry, even if the present set happens to contain an empty string).
- **INV-6 (source wiring)** — `roadmapdialog.cpp` captures on `closeEvent`
  (`captureScrollAnchor`) and restores on `showEvent`
  (`restoreScrollAnchor`), and round-trips through
  `Config::roadmapScrollAnchors` / `setRoadmapScrollAnchors`.
