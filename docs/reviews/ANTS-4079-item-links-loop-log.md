# ANTS-4079 item links — review record

The loop log for [`docs/specs/ANTS-4079-item-links.md`](../specs/ANTS-4079-item-links.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-29 | 2 (neutral-lane, each lane held every question) | 0 | 2 | 5 | 0 | Verified 7, fixed 7, dismissed 1 (a re-add pre-check the builder settles locally). Q2: the link-cycle refusal contradicted ANTS-3810 INV-2, now confined to the op:"link" handler with import restoring rows as stored; the rendered pass-headings `- **Key**:` line was outside the import grammar. Q3: whether import strips link lines from the body (it does); which body owns a shared relates-to row (kept while either endpoint declares it); unresolved ids dropped from the file (now rendered); cross-project parts in reverse lookups; a cross-project Dependencies value. One open question resolved clean (the three uniqueness indexes exist). Both lanes found the import-body and relates-to defects; lane 1 raised the cross-project questions as open. Span: the whole document is new, so no in-span share. |
