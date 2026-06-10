# ANTS-2058 — parseBullets populates `lanes` from inline metadata

Status: implemented

## Problem

`roadmap_query` (via `RoadmapDialog::parseBullets`) returned `lanes: []`
on every bullet whose metadata is written inline as one prose sentence
(`Kind: refactor. Lanes: backend tests. Source: …`) — even though it
extracted `kind: "refactor"` from the **same line**. Root cause: `rxLanes`
was `^\s*Lanes:…` with `MultilineOption`; the `^` anchor matched only a
line-leading `Lanes:`, so a mid-line clause never matched. `rxKind` worked
only because `Kind:` happened to sit at the line start. The asymmetry
(kind populated, lanes empty from the same sentence) defeated the
same-kind+same-lanes bundling the field exists for (MAME Curator MEDIUM,
2026-06-10).

## Surface

- `src/roadmapdialog.cpp` — `rxLanes` (drop the `^` line-start anchor).

## Invariants

- **INV-1** — Inline `Kind: X. Lanes: Y.` on one continuation line yields
  `lanes: ["Y"]` (and still `kind: "X"`).
- **INV-2** — A comma-separated inline list (`Lanes: a, b, c.`) splits into
  three trimmed entries.
- **INV-3** — Regression: a line-leading `Lanes:` on its own continuation
  line still parses (dropping `^` must not break the original form).

## Tests

`test_roadmap_query_inline_lanes.cpp` calls the pure static
`RoadmapDialog::parseBullets` directly and asserts the `lanes` field.
