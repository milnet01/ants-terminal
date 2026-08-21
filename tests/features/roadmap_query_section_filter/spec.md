# roadmap_query_section_filter — mode:section_index can be narrowed by name

**Item:** ANTS-4610 · **Bundle:** `test_claude` ·
**Suite:** `roadmap_query_section_filter` · **Label:** `features`

## Why this exists

`mode:section_index` is the step before the most common write — you call it to
learn the slug that `roadmap_log`'s `section` argument wants. It could not be
narrowed, so resolving one slug cost the whole index: **40 section objects,
~4.7k tokens, for one string.** On a mature roadmap most of those rows are
sections the caller will never use; the measured file had 154 shipped items in
sections whose `active_count` is 0.

Neither existing knob helps. `fields` operates on top-level response keys, so
`["sections"]` keeps the whole array. `compact` drops empty *values*, not rows.
The envelope echoed `filter:"all"`, which reads as though a narrower filter
exists, and the schema documented none for this mode.

The workaround was to look up a known id with `mode:headline_only` and read
`section_slug` back — ~90 tokens, but it only works when you already know an id
in the target section, which a session filing NEW work usually does not.

**An `active_only` flag would not have answered this.** The reporter's target
section had `active_count` 0. That is why the ask is a filter on the *name*.

Distinct from ANTS-1848 (status filter), ANTS-1729 (pagination) and ANTS-4467
(`slugs_only`), all shipped: none of them narrows by name.

## Design note — why `query` and not a new `q`

`query` already means "case-insensitive substring filter" on the bullets path,
and already carries `whole_word` and `regex`. Reusing it makes the narrowing
one concept with one spelling; a new `q` key would be a second dialect for the
same idea. What changes is that `query` + `mode:section_index` stops refusing
`bad_mode_combo` and starts filtering the sections instead.

The matcher is shared rather than copied: `mcp::textMatchesQuery` was lifted
out of `mcp::bulletMatchesQuery`, which now calls it. A second matcher would
drift from the first.

## Invariants

- **INV-1 — `query` narrows `sections[]`** instead of refusing.
- **INV-2 — a section with no active work still matches.** The measured case.
- **INV-3 — slug spelling resolves as well as headline spelling.** A caller who
  has seen a slug spells the slug.
- **INV-4 — zero matches is legible:** `query` echoes, and
  `sections_considered` / `sections_filtered_out` separate "filtered everything
  out" from "this roadmap has no sections". Same shape `file_outline`'s
  `filter` uses.
- **INV-5 — `whole_word` and `regex` compose,** with the bullets path's
  semantics, because it is the same matcher.
