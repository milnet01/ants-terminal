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

> **ANTS-2126 narrowing.** A real pass-headings writer replaced the
> refusal for `append` / `append_batch` / `flip` / `flip_batch` /
> `annotate` (those write now — see `mcp_roadmap_log_pass_writer`). Only
> `create_section` keeps refusing. The original INV-1 (append), INV-3
> (flip/annotate) and INV-4 (flip_batch) were removed, and INV-2 was
> narrowed from "append_batch + create_section" to create_section only.

### INV-2 — create_section refuses with format_mismatch

`cmdRoadmapLogCreateSection` on a pass-headings roadmap returns
`{ok:false, code:"format_mismatch", format:"pass-headings"}` — section
creation on the heading format is out of scope for ANTS-2126.

### INV-5 — ants-v1 roadmaps are unaffected

The gate keys on the parsed-bullet format, so a normal ants-v1 roadmap
still appends successfully (no false refusal).

### INV-6 (ANTS-2048) — stray `- [ ]` does not flip detection to gfm

A pass-headings roadmap carrying stray `- [ ]` checkbox sub-tasks is
still classified `pass-headings`. Under the ANTS-2126 writer that shows
as `flip_batch` routing to the pass writer and flipping `PASS-41-5`
(`format:"pass-headings"`, `flipped_count:1`); a mis-detection to
github-task-list would instead skip it as `bullet_not_found`.

## Test plan

Behavioural test via the `*ForTest` seams against a seeded
pass-headings `ROADMAP.md` in a `QTemporaryDir` (mirrors
mcp_roadmap_log_atomicity). INV-5 reuses the ants-v1 seed to confirm
the happy path is intact.
