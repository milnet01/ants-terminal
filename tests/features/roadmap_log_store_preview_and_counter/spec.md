# roadmap_log — the store branch's preview keys and counter cache

Contract for ANTS-4634 and ANTS-4635. Both are defects in `roadmap_log`'s
**store-backed** write branch, and both existed because every test for the
behaviour they break drives the **markdown** branch.

That is the point of this suite. `roadmap_log` has two backends and they are
asserted very unevenly: the markdown branch is exercised by
`roadmap_log_prefix_and_dry_run`, while the store branch — which every
migrated project on this machine takes, all fourteen of them — had no case
covering either rule. A green suite therefore said nothing about the path
almost every real call goes down.

## INV-1 — a store-backed preview reports `would_be_id`, never `id`

`op:"append"` with `dry_run:true` against a migrated project emits the
would-be id under `would_be_id`. The key `id` is **absent**.

ANTS-4508 established the rule and the reason: a caller reading a single
field takes the real write's key for a reservation, and nothing is reserved —
the id is wrong if any other write intervenes, in a way nothing detects. That
rename landed on the markdown branch only. Reported by finbreak on 2026-08-24
and reproduced directly against Ants_Terminal, which returned
`{"dry_run":true,"id":"ANTS-4634",...}`.

## INV-2 — a store-backed real write still reports `id`

The rename is preview-only. A real `op:"append"` emits `id` and **not**
`would_be_id`, so the two envelopes stay distinguishable by the key that
carries the id. This is the invariant that stops INV-1 being satisfied by
renaming the field everywhere.

## INV-3 / INV-4 — the same two rules for `op:"append_batch"`

A store-backed batch preview emits `would_be_ids` and no `ids`; a real batch
emits `ids` and no `would_be_ids`.

## INV-5 — a store-backed batch reconciles `.roadmap-counter`

After a real `op:"append_batch"`, `.roadmap-counter` holds the highest id the
batch allocated, and the envelope reports `counter_advanced_to` /
`counter_advanced_past`.

`op:"append"` has done this since ANTS-4141 part 2; the batch path never did,
so the cache stayed stale until some later single append's scan repaired it.
Games_Hub measured three batches allocating GHUB-0113 through GHUB-0118 with
the counter still reading 110.

Nothing is lost when it drifts — the scan is a genuine safety net — so the
invariant is about the *cache being trustworthy*, not about allocation
correctness. Anything that reads `.roadmap-counter` to predict the next id (a
script, a hook, a fresh clone, an un-migrated sibling) gets a wrong answer
that looks right, and two concurrent sessions both start from the stale
value.

## Out of scope

The counter's continued existence. ANTS-4632 proposes retiring
`.roadmap-counter` once nothing outside this repo reads it; if that lands,
INV-5 goes with it. Until then the two paths must agree, and this suite
asserts that they do — the defect was the disagreement, not the mechanism.
