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

## Tests

A pytest fixture (8 `test_*.py` files each containing `time.sleep(`, which
the `sleep_call` pre-pass pattern matches) chunked at `chunkSize=4` yields
two chunks, each carrying pre-pass findings. `partition(offset=1, limit=1)`
returns page two (`c-002`); `brief("c-002")` must return non-empty
`pre_pass_findings` (INV-1). INV-2 is a source guard that the envelope
inline is gated on `!r.prePassCached`.
