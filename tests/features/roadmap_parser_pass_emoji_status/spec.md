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

### INV-5 — a Status line anywhere in its block is found (ANTS-5337)

The reader takes the first `- **Status**:` line between the `#### Pass`
heading and the next heading of level ≤ 4, however far down it sits.
RetroDB's PASS-57-1 carries its only Status line 61 lines into its block;
the old 50-line window missed it and the item migrated as open.

### INV-6 — a flip rewrites that same line (ANTS-5337)

`PassHeadingWrite::flipPassStatus` scans the same span as the reader, so it
rewrites the late line rather than inserting a second Status line under the
heading. The two scans must stay identical: a writer window narrower than the
reader's is how a flip adds a line the reader then ignores.

## Test plan

Behavioural test against `RoadmapDialog::parseBullets`, which
dispatches to `parsePassHeadingBullets` when the doc is detected as
`pass-headings` format (≥2 `#### Pass` headings + ≥2 `- **Status**:`
markers, no ants-v1 emoji). Synthetic fixtures, no real ROADMAP.md.
The test FAILS against pre-fix code (the emoji-prefixed headings read
📋).
