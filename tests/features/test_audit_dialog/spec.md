# test_audit_dialog — feature-conformance contract

Tracks `docs/specs/ANTS-1722.md` (`TestAuditDialog : ReviewDialogBase`).
Exercises the non-GUI partition / brief / synthesis / fold-in / resume
paths — `TestAuditEngine`, `BriefDispatch`, `SessionMemoryEngine`, the
ROADMAP fold-in — not the live Qt event loop. INV-8 is a construction
smoke assertion.

## Asserts

- **INV-1** — `derivePartition()` chunks mirror
  `TestAuditEngine::partition`; `partitionToken()` is the engine token
  and every `brief` request carries it (a successful `briefFor(chunkId)`
  proves the token routed through).
- **INV-2** — `composeBrief(lane)` seeds the user prompt with the full
  `dimensionsActive` list (not just pre-pass hits) and inlines the
  chunk's `sourcePaths` fenced via `BriefDispatch::inlineBodies`.
- **INV-3** — `onAllReportsCollected` writes each verbatim chunk report
  under `.audit_cache/test_audit_<token>/` and calls `synthesize` with
  that reports dir; `lastSynth().reportsRead` equals the chunk count.
- **INV-4** — the composed user prompt is capped at `kPromptCapBytes`
  (200 KiB) with a truncation marker when a chunk's inlined bodies
  exceed it.
- **INV-5** — `performFoldIn()` in `Narrative` mode allocates no IDs
  (`.roadmap-counter` unchanged); in `PerFinding` mode it allocates one
  ID per actionable finding (counter advances by N).
- **INV-6** — a `stale_partition` code from `brief` (post-restart cache
  miss) triggers a `partition` re-run rather than a hard error; collected
  reports are preserved across the re-partition.
- **INV-7** — `loadResume()` reads `{ partition_token,
  collected_chunk_ids }` from `session_memory`; `unreviewedChunkIds()`
  returns only the chunks not already collected.
- **INV-8** — constructing the dialog with an unset/invalid `ai_endpoint`
  leaves dispatch disabled; the dialog constructs without crashing.

Note: the spec names the cache-miss code `stale_token`; the engine
actually returns `stale_partition` — the dialog (and this test) use the
real value.
