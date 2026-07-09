# roadmap_fold_in — RoadmapFoldIn helpers (ANTS-1111)

Locks RoadmapFoldIn::allocateIds, insertBlock, findActiveReleaseHeading,
and AuditEngine::templateRoadmapFoldInBlock.

## INVs

- INV-5: `templateRoadmapFoldInBlock` emits `### 🔍 Audit fold-in (DATE)`
  as the first line; first bullet starts with `- 📋 [ANTS-<id>]`.
- INV-6: `RoadmapFoldIn::allocateIds(path, n)` returns N consecutive
  ints starting from current+1; bumps `.roadmap-counter` atomically.
  Concurrent calls under flock get disjoint ranges (single-process
  serialised here; flock-collision tested separately).
- INV-6b (ANTS-3450): `RoadmapFoldIn::corpusHighWater(path[, prefix])`
  returns the highest `<prefix>-NNNN` id across the committed corpus —
  ROADMAP.md + CHANGELOG.md + `docs/roadmap/*.md` — sniffing the project's
  dominant prefix when none is given; `0` for a bad root or a prefix with
  no ids. `allocateIds` (and `peekIds`) **floor** their starting point to
  this value, so the now-untracked `.roadmap-counter` cache — stale, wiped,
  or fresh-clone absent (→ 0) — can never make the fold-in path reissue a
  live or migrated id. Verified by `CorpusHighWaterAcrossSources`: a
  stale counter (100) below ids seeded into each of the three files
  allocates above the true max (250), not the counter+1.
- INV-7: `insertBlock` is atomic — temp-file pattern preserved by
  QSaveFile. Verified by reading the file post-write and checking
  the block lands on the line after the named heading.
- INV-8: `insertBlock(path, heading, block)`:
  (a) heading found → block inserted on the line after.
  (b) heading not found → return false, file unchanged byte-for-byte.
- INV-8b: `findActiveReleaseHeading` prefers `(target: …)` headings;
  falls back to most-recent shipped; returns "" when no recognisable
  release block exists.
