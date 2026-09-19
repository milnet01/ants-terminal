# Feature: roadmap_log deletes and moves sections

## Invariants

**INV-1 — `delete_section` removes an emptied section.** It takes
`section`. The section's intro and any narration or table go with it,
and the reply returns them as `removed_intro` and `removed_elements`.

**INV-2 — it refuses while the section files an item.** The code is
`section_not_empty`, `item_ids` names the items, and nothing is written.

**INV-3 — it refuses a section with subsections.** The code is
`section_has_subsections`.

**INV-4 — `move_section` moves a section with its subsections.** It takes
`section` and exactly one of `after_section` or `before_section`. After an
anchor, it steps past the anchor's deeper sections, as `create_section`
does, so it never adopts another section's children. Items move with
their section.

**INV-5 — a move that would change nesting is refused.** Putting a section
before a deeper one, or next to a section inside itself, refuses
`bad_args` and writes nothing. So does passing both anchors.

Both ops are store-only.

## Rationale

ANTS-4958 and ANTS-4922. Re-sectioning a roadmap with the ANTS-4948 move
left the emptied source section behind, and nothing could remove or
reorder a section. The user decided on 2026-09-19 that explicit ops own
structure, rather than a re-import pruning it.

## Test surface

`test_roadmap_log_section_ops.cpp` migrates a fixture into a sandboxed
store and drives the two `*ForTest` seams.

## Regression history

- **ANTS-4958:** no op deleted or reordered a section.
