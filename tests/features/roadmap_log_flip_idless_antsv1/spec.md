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

- the remotecontrol TUs — `walkAntsV1Bullets`.

## Invariants

- **INV-1** — A fully id-less ants-v1 bullet (`- 📋 **Headline.**`, no
  `[…]`) is parsed; `op:flip` located by `headline` flips its emoji and
  returns `ok:true` (was `unrecognised_format`). The bullet's `id` is empty.
- **INV-2** — `op:flip_batch` with a `line_range` locator flips every
  id-less bullet in range in one commit.
- **INV-3** — Regression: a mixed file (one id-ful `[ANTS-NNNN]` bullet +
  one id-less bullet) still resolves the id-ful bullet by `id` and the
  id-less bullet by `headline`; neither is dropped.
- **INV-4** (ANTS-4109) — An ants-v1 bullet whose id is **bold** rather
  than bracketed (`- 📋 **LOTTO-0019** …`) resolves by the `id` locator.
  The bracket was the only id shape `walkAntsV1Bullets` knew, so these
  bullets came back id-less and `op:flip {id}` refused `bullet_not_found`
  for every bullet in the file — while `roadmap_query {id}` resolved the
  same string, because the read path extracts it. The write path now
  mirrors `fillBulletRecord`'s native branch: the leading bold token is
  adopted **only when ID-shaped** (a single whitespace-free token), so a
  bold-prose narrator stays id-less (INV-1 is the guard on that). The
  bullet's `headline` locator keeps working — the change is additive.
- **INV-5** (ANTS-4109) — `op:flip_batch` in which **every** locator fails
  to resolve returns `ok:false` with a `code`, not `ok:true` with
  `flipped_count:0`. Partial success stays `ok:true` (INV-2); with nothing
  applied there is no rest to still apply, and a caller not reading
  `flipped_count` reported a bundle shipped that was still planned.
  `skipped[]` / `skipped_count` are unchanged.

## Tests

`test_roadmap_log_flip_idless_antsv1.cpp` drives
`cmdRoadmapLogFlipForTest` / `cmdRoadmapLogFlipBatchForTest` against
seeded temp roadmaps padded past `kRoadmapMinParseableSize`.
