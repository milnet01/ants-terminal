# ANTS-2031 — roadmap_log refuses `#### Pass N.M` heading roadmaps with format_mismatch

## Background

`roadmap_log`'s writers emit GFM / ants-v1 bullets. A `#### Pass N.M`
heading roadmap (ANTS-1530) parses fine on the READ side, so the
`unrecognised_format` gate (which fires only on zero parsed bullets)
never trips — and an unguarded `op:append` would splice a GFM/ants-v1
bullet into a heading file, corrupting its format. RetroDB therefore
used the Edit tool for both whole-pass appends and sub-bullet status
flips. The bullet asks for, at minimum, a `format_mismatch` warning so
the caller knows to fall back to Edit.

## Invariants

### INV-1 — append refuses with format_mismatch

`cmdRoadmapLogAppend` on a pass-headings roadmap returns
`{ok:false, code:"format_mismatch"}` with a `hint` steering the caller
to Edit and `format:"pass-headings"`. It does NOT mutate the file.

### INV-2 — append_batch / create_section refuse identically

Both splice paths return `code:"format_mismatch"` on a pass-headings
roadmap.

### INV-3 — flip / annotate refuse with format_mismatch

`cmdRoadmapLogFlip` (which serves both `flip` and `annotate`) returns
`code:"format_mismatch"` on a pass-headings roadmap instead of the
misleading `unrecognised_format` / `bullet_not_found`.

### INV-4 — flip_batch refuses with format_mismatch

`cmdRoadmapLogFlipBatch` returns `code:"format_mismatch"`.

### INV-5 — ants-v1 roadmaps are unaffected

The gate keys on the parsed-bullet format, so a normal ants-v1 roadmap
still appends successfully (no false refusal).

## Test plan

Behavioural test via the `*ForTest` seams against a seeded
pass-headings `ROADMAP.md` in a `QTemporaryDir` (mirrors
mcp_roadmap_log_atomicity). INV-5 reuses the ants-v1 seed to confirm
the happy path is intact.
