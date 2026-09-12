# test_audit pagination keeps pre-pass findings reachable via brief (ANTS-2096)

## Problem

`TestAuditEngine::partition()` caches the partition so `test_audit_brief`
can serve each chunk's pre-pass findings without re-walking. On a
paginated call (`offset > 0`) the old code set `prePassCached = true` and
then **cleared** `prePassFindingsByChunk` on the *same* `PartitionResult`
that `cachePartition()` stores. `brief()` reads its findings from that
cache (`p->prePassFindingsByChunk.value(chunk->id)`), so every chunk on
page 2+ briefed back an **empty** `pre_pass_findings`.

The clear conflated two concerns: omitting the bulky map from the
serialized envelope (correct, token-saving) vs. retaining it in the cache
for `brief` (required).

## Invariants

- **INV-1** — After `partition(offset>0, limit>0)`, `brief()` on a chunk
  returned in that page still returns its non-empty `pre_pass_findings`
  (the cache is not emptied for page 2+).
- **INV-2** — The serialized envelope still omits the inline
  `pre_pass_findings_by_chunk` map for a `prePassCached` (page 2+) result,
  so the token-saving of ANTS-2070 is preserved (the map lives in the
  cache, not on the wire).
- **INV-3** — A partition run with `prePass = false` derives the same
  `partitionToken` and the same chunk set (ids and paths) as one run with
  the pre-pass, and carries no `prePassFindingsByChunk`. The dialog derives
  the token on its refresh path and dispatches against it later, so a
  token that differed there would strand every brief.
- **INV-4** — A partition run with `prePass = false` seeds the cache when
  the token is absent, but never displaces an entry already there.
  `brief()` still returns that chunk's non-empty `pre_pass_findings` after
  a lean partition for the same token. It has to seed, because synthesis
  looks the partition up by token and the dialog's panel refresh is what
  seeds it on open; it must not displace, because replacing a full entry
  with a lean one empties every brief — INV-1's failure reached by another
  route.

## Tests

A pytest fixture (8 `test_*.py` files each containing `time.sleep(`, which
the `sleep_call` pre-pass pattern matches) chunked at `chunkSize=4` yields
two chunks, each carrying pre-pass findings. `partition(offset=1, limit=1)`
returns page two (`c-002`); `brief("c-002")` must return non-empty
`pre_pass_findings` (INV-1). INV-2 is a source guard that the envelope
inline is gated on `!r.prePassCached`.

INV-3 and INV-4 (ANTS-5126) use the same fixture, comparing a
`prePass = false` partition against a default one and then briefing
against the cached token. The flag exists because the pre-pass reads and
regex-scans every file in every chunk, and `TestAuditDialog` ran the whole
of `partition` on the GUI thread on dialog open and on every
`editingFinished`. Measured by `tests/perf/bench_partition_walk` on this
project: about 430 ms with the pre-pass against about 23 ms without, where
ANTS-1397 § 6 accepts the GUI-thread placement only while the call fits
roughly 50 ms. The directory walk is a small part of that cost, so moving
the call to a worker would have hidden it rather than removed it.
