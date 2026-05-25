# roadmap_log op:"flip_batch" — Feature Spec (ANTS-1690)

## Purpose
Flip N ROADMAP bullets to one `to_status` in a single read + single
atomic QSaveFile commit, so a bundle close (cold-eyes / indie-review /
release sweep) doesn't pay N round-trips or race the file watcher across
N writes. Each locator is `{id|anchor|headline|line_range}` + optional
per-locator `note` and `no_anchor`.

## Invariants

- **INV-1** — `op:"flip_batch"` flips every located bullet to `to_status`
  in one commit; the response is `{ok:true, op:"flip_batch",
  flipped:[…], flipped_count:N, skipped:[…]}` and each named bullet's
  status emoji is rewritten in the file.
- **INV-2** — index stability: when several bullets each carry a
  per-locator `note`, every bullet flips AND each note lands in its own
  bullet's body (no cross-contamination) — the handler applies in
  descending line order so a note insertion never shifts a
  not-yet-applied target.
- **INV-3** — partial success: an unresolvable locator lands in
  `skipped[]` with a `code` (`bullet_not_found` / `bullet_ambiguous` /
  `missing_field`) while every resolvable locator still applies;
  `flipped_count + skipped_count` accounts for all inputs.
- **INV-4** — refuses `missing_field` when `locators` is absent/empty or
  `to_status` is absent; refuses `bad_status` on an unknown `to_status`.
- **INV-5** — a `line_range:[start,end]` locator flips every bullet whose
  1-based line falls inside the inclusive range.
- **INV-6** — on a GFM bullet with no id and no existing anchor, a caret
  anchor is injected and `.roadmap-counter` is consumed; across a batch
  the counter is written exactly once (single atomic commit). A locator
  with `no_anchor:true` flips without injecting.
- **INV-7** — dedup: a bullet referenced by two locators flips exactly
  once (it appears once in `flipped[]`).

## Test
`tests/features/roadmap_log_flip_batch/` (label `features;fast`), driving
`RemoteControl::cmdRoadmapLogFlipBatchForTest` against seeded temp
ROADMAPs, plus a source-grep for the dispatch + schema surface. Verify
each behavioural test fails against pre-ANTS-1690 source (the verb did
not exist).
