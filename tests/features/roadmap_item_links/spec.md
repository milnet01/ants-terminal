# Feature: roadmap item links (ANTS-4079)

The contract is `docs/specs/ANTS-4079-item-links.md`, accepted 2026-09-29.
This test discharges its INV-1 to INV-10; INV-11 is in
`tests/features/roadmap_log_op_enum`. Each case migrates small markdown
fixtures for two projects, `DEMO` and `VEST`, into one sandboxed store, then
drives `roadmap_log`, `roadmap_query` and `feedback_query` through
`RemoteControl`.

- **INV-1** `op:"link"` writes a row. `roadmap_query id:` reports it under
  `links`, and the target's own fetch reports it under the reverse key.
  `op:"unlink"` removes it. A repeat of either lands in `unchanged`.
- **INV-2** A link to an unfiled `DEMO` id refuses `link_target_not_found`
  and writes nothing. A link to an unfiled `VEST` id succeeds as a
  cross-project row.
- **INV-3** An edge that would close a same-type cycle refuses `link_cycle`
  and names the cycle. The same pair under another type succeeds.
- **INV-4** Flipping a parent to `shipped` while a `splits-from` part is
  open refuses `open_parts`. It succeeds once the part is `shipped` or
  `dropped`. In `flip_batch` the refusal is per locator.
- **INV-5** A flip of an item with an open `blocked-by` target succeeds
  and carries `blocked_by_open`.
- **INV-6** Render then re-import restores the same rows and the same
  file, on ants-v1 and pass-headings. The cases include all four authored
  types, one unresolved id and one same-type cycle. A line starting with a
  link key whose value is not an id list stays prose, where it was written.
- **INV-7** Migration turns `Dependencies:` and `Spec:` lines into
  `relates-to` and `specified-by` rows. It keeps an unresolvable value in
  `extras`, and leaves the rendered body byte-identical.
- **INV-8** Removing a `Dependencies:` value with `amend_body` removes its
  row. The row stays while the other endpoint still declares the pair.
- **INV-9** A body write declaring a link line refuses `body_shadowed`;
  prose after the key does not.
- **INV-10** `feedback_query` reports a cited parent's open part in
  `mapped_id_parts`, and `mapped_id_status` keeps the parent's own status.

*Test:* `test_roadmap_item_links.cpp`.
