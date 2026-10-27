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
  `missing_field` / `duplicate_locator`) while every resolvable locator
  still applies;
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
  once (it appears once in `flipped[]`). The later locator is not dropped:
  it lands in `skipped[]` with `code:"duplicate_locator"` and an `error`
  naming the earlier locator index ("locator 0"), whether it repeats the id
  or names the same bullet by another key (e.g. a headline), so
  `flipped_count + skipped_count` still equals the locator count. A
  `line_range` overlapping earlier locators is a duplicate only when every
  bullet it matched was already claimed.
- **INV-8** — `dry_run:true` resolves the locators and writes nothing.
- **INV-9** — `to_status` (call-level and per-locator) accepts the same
  synonyms as `op:"flip"`, through the same map (`rlCanonicalToStatus`:
  done/complete/completed → shipped, wip/in_progress → in-progress,
  todo/open → planned, maybe/idea → considered), besides the canonical
  words and emoji. Any other word is still `bad_status`.

## Test
`tests/features/roadmap_log_flip_batch/` (label `features;fast`), driving
`RemoteControl::cmdRoadmapLogFlipBatchForTest` against seeded temp
ROADMAPs, plus a source-grep for the dispatch + schema surface. Verify
each behavioural test fails against pre-ANTS-1690 source (the verb did
not exist).
