# roadmap_item_removal — refusing a duplicate append (ANTS-4487)

Parent: [`docs/specs/ANTS-4487-item-removal.md`](../../../docs/specs/ANTS-4487-item-removal.md)
§ 4.5 and INV-8.

## Contract

- **INV-8** — `roadmap_log op:"append"` refuses a headline that exactly
  matches an existing item (the near-duplicate advisory's score 100) with
  `duplicate_item`, naming the match in `duplicate_of`, and writes nothing:
  no bullet, no counter advance. `force:true` files it. A dry run refuses
  where the real run would. `op:"append_batch"` skips such an entry into
  `skipped[]` with the same code and applies the rest. Both the markdown
  path and the store path hold it.

Below score 100 the advisory stays advisory; tests in
`roadmap_log_possible_duplicates` cover that, forcing past this refusal
where they need an exact match.

Cases `removeRefusesRenderedId`, `removeRefusesReferenced`,
`removeLeavesNoOrphans` and `removeDryRunIsInert` (INV-4..7) land with
`op:"remove"`, which waits on a user decision.
