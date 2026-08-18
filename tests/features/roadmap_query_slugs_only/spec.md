# Feature: `slugs_only` on roadmap_query mode:"section_index"

## Contract

`roadmap_query {mode:"section_index", slugs_only:true}` returns a flat
`slugs` array of slug strings in place of the `sections` array of
objects, plus `slugs_only:true` and a `total`. Same status filter, same
drop rules, same ordering as the object form — only the row shape
changes.

## Rationale

ANTS-4467, reported by Vestige. `section_index` exists to hand back the
slugs that `roadmap_log`'s `section` argument requires — that verb's own
documentation says so. On an 84-section roadmap the object form exceeded
the inline budget and offloaded, and the spill keeps whole bodies for a
PREFIX of rows and shape-only `{index, bytes}` stubs for the rest. That
is the worst possible split for an index-shaped reply whose caller needs
one key from every row: the call cost ~20 KB and returned slugs for 8 of
84.

So the documented prerequisite for appending a bullet broke on exactly
the projects large enough to need an index. The reporter fell back to
`grep -n '^## \|^### '`, which global rule 18 asks sessions to avoid and
which was strictly cheaper — that is the part that made this worth
fixing rather than documenting. `fields:["sections"]` does not help: it
keeps whole section objects, and the spill is driven by row bodies
rather than by sibling top-level keys.

## Invariants

**INV-1 — every slug is returned.** A roadmap with N sections returns N
slugs, in document order, matching the slugs the object form emits.

**INV-2 — the section objects are absent.** `sections` is not emitted
alongside `slugs`; a caller gets one shape or the other, so there is no
question of which to read.

**INV-3 — the status filter still applies.** `slugs_only` is a
projection, not a mode: `status:"active"` drops the same sections it
drops in the object form.

**INV-4 — it is inert outside section_index.** Passing `slugs_only` to
the bullets path changes nothing.

## Scope

### In scope
- The `slugs` projection and its interaction with the status filter.

### Out of scope
- Making the SPILLER degrade by dropping fields rather than rows, which
  is the reporter's more general fix and would serve every index-shaped
  reply (compare workspace_search's `downshifted` path, ANTS-3543). Left
  open on ANTS-4467 deliberately: this projection removes the pain on the
  reported path, and the general fix is a separate piece of work with a
  wider blast radius.
- Dotted `fields:["sections.slug"]`, the reporter's cheap interim. The
  projection supersedes it for this mode.
