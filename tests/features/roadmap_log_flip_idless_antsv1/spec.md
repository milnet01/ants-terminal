# ANTS-2059 — roadmap_log accepts fully id-less ants-v1 bullets

Status: implemented

## Problem

`roadmap_log` op:`flip` / `flip_batch` / `annotate` refused a legacy
`ants-v1` roadmap whose bullets carry a status emoji but **no
`[PROJ-NNNN]` id token at all** (`- 📋 **Headline.**`). The write-path
walker `walkAntsV1Bullets` required the character after the emoji + space
to be `[`, so an id-less bullet was skipped, the whole-file walk returned
zero, and every mutating verb refused with `unrecognised_format` — even
though the READ path (`parseBullets` / `roadmap_query`) synthesises an id
for these bullets and reads them fine. ANTS-2051 relaxed the bracket's
leading-letter case (lowercase prefixes) but kept the bracket *mandatory*,
leaving the fully id-less case (RetroArch HIGH + Album Builder,
2026-06-10) blocked. The close-the-bundle flow fell back to hand-`Edit`.

## Surface

- `src/remotecontrol.cpp` — `walkAntsV1Bullets`.

## Invariants

- **INV-1** — A fully id-less ants-v1 bullet (`- 📋 **Headline.**`, no
  `[…]`) is parsed; `op:flip` located by `headline` flips its emoji and
  returns `ok:true` (was `unrecognised_format`). The bullet's `id` is empty.
- **INV-2** — `op:flip_batch` with a `line_range` locator flips every
  id-less bullet in range in one commit.
- **INV-3** — Regression: a mixed file (one id-ful `[ANTS-NNNN]` bullet +
  one id-less bullet) still resolves the id-ful bullet by `id` and the
  id-less bullet by `headline`; neither is dropped.

## Tests

`test_roadmap_log_flip_idless_antsv1.cpp` drives
`cmdRoadmapLogFlipForTest` / `cmdRoadmapLogFlipBatchForTest` against
seeded temp roadmaps padded past `kRoadmapMinParseableSize`.
