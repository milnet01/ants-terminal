# session_orient active_bullets fallback on id-less roadmaps (ANTS-2052)

On a fully id-less ants-v1 roadmap (every `- 📋 **…**` bullet, no
`[PROJ-NNNN]`), the default id filter drops every active bullet, so
`session_orient`'s `active_bullets` returned `count:0` and the flagship
"what should I work on?" bundle read as "no active work" — even with many
open items. ANTS-1848/2052 already taught `mode:section_index` to emit
`legacy_format` + `raw_active_count`; this completes the orient path.

## Invariant

- **INV-1** — When `active_bullets` (headline_only / status:active) returns
  zero on a legacy roadmap whose `section_index` reported a non-zero
  `raw_active_count`, `session_orient` re-issues the query with
  `include_narrator_bullets:true` and flags the recovery
  (`legacy_format_fallback:true` + `raw_active_count` + a hint), so the
  session sees the open items rather than an empty queue.

## Out of scope

The GUI Roadmap dialog's own rendering of id-less bullets; teaching the
roadmap to adopt `[PROJ-NNNN]` ids (a roadmap-format migration).
