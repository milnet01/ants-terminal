# ANTS-2039 — pass-headings status classifier reads the emoji-prefixed `✅ Done` form

## Background

`roadmap_query` / `session_orient` read `#### Pass N.M …` heading
roadmaps via `RoadmapDialog::parsePassHeadingBullets` (ANTS-1530). The
status emoji is derived from the first `- **Status**: <word>` line
under each heading. The capture regex `rxStatusLine` matched
`([A-Za-z0-9_-]+)` immediately after `- **Status**:` plus whitespace,
so a **leading status emoji** (`✅`/`📋`/`🚧`/`💭`) — neither a word
char nor whitespace — blocked the capture: `statusWord` stayed empty
and defaulted to planned (📋).

RetroDB session-5 confirmed the symptom: all of `PASS-48-1..5` read 📋
despite `- **Status**: ✅ Done (v3.6.3x)` lines; rewriting
`✅ Done (` → `done (` on disk flipped all five to ✅ on re-query.

This is a **reader** bug — distinct from the writer gap ANTS-2031 (the
file was already correct; the parser misread it). Bare-keyword forms
(`done (…)`, `shipped in …`) already classify fine.

## Invariants

### INV-1 — emoji-prefixed `✅ Done` reads as shipped

`parsePassHeadingBullets` on a heading followed by
`- **Status**: ✅ Done (v3.6.3x)` yields a bullet with status `✅`
(not 📋). The `(v3.6.3x)` trailing parenthetical does not perturb the
keyword capture.

### INV-2 — every status emoji is skipped before the keyword

The leading-emoji skip is general: `🚧 In-progress` → 🚧,
`💭 Deferred` → 💭, `📋 Todo` → 📋.

### INV-3 — a bare emoji with no trailing keyword is authoritative

`- **Status**: ✅` (no following word) reads as `✅` — the emoji maps
directly when no keyword follows.

### INV-4 — the bare-keyword forms are unchanged

`- **Status**: done` (no emoji) still reads as `✅`, and
`- **Status**: in-progress` still reads as 🚧 — the fix is additive
and does not perturb the established keyword classification.

## Test plan

Behavioural test against `RoadmapDialog::parseBullets`, which
dispatches to `parsePassHeadingBullets` when the doc is detected as
`pass-headings` format (≥2 `#### Pass` headings + ≥2 `- **Status**:`
markers, no ants-v1 emoji). Synthetic fixtures, no real ROADMAP.md.
The test FAILS against pre-fix code (the emoji-prefixed headings read
📋).
