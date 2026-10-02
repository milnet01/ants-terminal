# roadmap_log element ops — list, amend, delete, promote (ANTS-5379), and the legend (ANTS-5615)

A section holds items, tables and narration as ordered elements, keyed by
position within the section. The intro ops (`set_intro`, `set_preamble`)
leave elements alone, and a hand edit to the rendered file is discarded by
the next render, so until these ops no route reached a narration element.
Store-backed projects only, like the other section ops.

- `op:"list_elements"` — `section`, or `preamble:true` for the root. Returns
  `elements[]` in position order: `{position, kind}` plus `text` for
  narration, `id` for an item, `rows` for a table.
- `op:"amend_element"` — `section` / `preamble`, `element_position`, `new_text`.
  Replaces a narration element's text.
- `op:"delete_element"` — `section` / `preamble`, `element_position`. Removes a
  narration or table element; echoes the removed narration as `removed_text`.
- `op:"promote_element"` — `section`, `element_position`, and
  `op:"append"`'s fields. Files a new item AT that position and removes the
  narration, in one write. `headline` defaults to the narration's first line
  without its list marker.

All four take `dry_run`. Reload: the verbs run in ants-mcpd, so a rebuild and
an MCP reconnect make them live; the terminal is not relaunched.

## Invariants

- **INV-1** — `list_elements` reports every element in position order, with
  the kind-specific key above. *Test:* `ListsInPositionOrder`.
- **INV-2** — `amend_element` replaces a narration element's text, and the
  render publishes it. It refuses an item or table element
  (`element_kind_refused`), an absent position (`element_not_found`), and
  text holding a `#` to `###` heading line (`bad_element_text`), writing
  nothing. *Test:* `AmendReplacesNarration`, `AmendRefusals`.
- **INV-3** — `delete_element` removes a narration or table element and
  refuses an item (`element_kind_refused`); a dry run writes nothing.
  *Test:* `DeleteRemovesNarrationNotItems`, `DeleteDryRunWritesNothing`.
- **INV-4** — `promote_element` files one item at the narration's position,
  with an allocated id, and the narration is gone; a non-narration element
  refuses `element_kind_refused`. *Test:* `PromoteFilesItemInPlace`.

- **INV-5** — (ANTS-5615) The status legend is not an element.
  `list_elements preamble:true` returns it as `legend` with a
  `legend_hint` naming `op:"set_legend"`. `set_legend` takes `legend`, a
  `{status: wording}` object: named statuses change, others keep their
  line, and an empty string removes one. It refuses an unknown status or a
  wording the import would not read back as a legend line (`bad_args`), and
  an empty `legend` (`missing_field`); a refusal or dry run writes nothing.
  *Test:* `SetLegendChangesOneStatusWording`,
  `SetLegendRefusalsAndDryRunWriteNothing`.

## Test

`tests/features/roadmap_log_elements/test_roadmap_log_elements.cpp`, in the
`test_claude` bundle, through `RemoteControl::cmdRoadmapLog`.
