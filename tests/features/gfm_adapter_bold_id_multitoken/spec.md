# Feature spec: GFM-adapter bold-ID widening (ANTS-1438)

`extractBoldId` previously required a single-token bold prefix with
a trailing period inside the bold (`**Sh4.**`, `**VEST-0042.**`).
Vestige's GFM-task-list bullets carry multi-token bold prefixes
without trailing periods (`**FW W5 (cont.)**`, `**Audit/FW X2**`,
`**Terrain System**`). The strict regex returned false for every
Vestige bullet → `id` was a 10-char content-hash nonce instead of
the human-readable label.

## Invariants

- **INV-1 / widened regex matches multi-token bold prefixes.**
  `extractBoldId` returns true for `**FW W5 (cont.)**`,
  `**Audit/FW X2**`, `**Terrain System**`, `**Sh4**` (no trailing
  period). Captured token is the bold content with trailing `.`
  stripped. Source anchor: `ANTS-1438` in
  `src/roadmapdialog.cpp::extractBoldId`.
- **INV-2 / trailing period stripped.** `**Sh4.**` yields
  `boldId == "Sh4"` — same as ants-style projects always saw.
- **INV-3 / native path untouched.** A bullet whose head doesn't
  start with `**` (ants-v1: `- ✅ [ANTS-NNNN] **Title.**`) does not
  call extractBoldId in the GFM branch; rxBold-based headline
  extraction runs unchanged.
- **INV-4 / em-dash separator splits headline from bold-ID.** When
  `boldId` is non-empty AND `head` contains ` — ` (or ` -- ` /
  ` - `), `rec.headline` is the trimmed text after the separator.
  Source anchor: `ANTS-1438-INV-4`.
- **INV-5 / no separator falls back to existing behaviour.** A GFM
  bullet with bold-ID but no em-dash falls through to the existing
  rxBold-based headline derivation.
- **INV-6 / `bold_id` envelope field present when boldId set.**
  Full-file, section, and section_index lazy-fill paths in
  `cmdRoadmapQuery` all emit `bold_id` on bullets with non-empty
  `BulletRecord::boldId`. Source anchor: `ANTS-1438 — bold_id`.
- **INV-7 / synthetic suppression.** When boldId is non-empty,
  `rec.synthetic` stays false. The synthetic content-hash branch
  fires only when both `[ANTS-NNNN]` and boldId are absent.
- **INV-8 / Vestige fixture round-trips.** Synthetic markdown
  modelled on Vestige's exact bullets parses into records with
  Vestige's expected bold-IDs.
