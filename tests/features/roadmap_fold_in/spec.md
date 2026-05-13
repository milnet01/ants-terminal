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
- INV-7: `insertBlock` is atomic — temp-file pattern preserved by
  QSaveFile. Verified by reading the file post-write and checking
  the block lands on the line after the named heading.
- INV-8: `insertBlock(path, heading, block)`:
  (a) heading found → block inserted on the line after.
  (b) heading not found → return false, file unchanged byte-for-byte.
- INV-8b: `findActiveReleaseHeading` prefers `(target: …)` headings;
  falls back to most-recent shipped; returns "" when no recognisable
  release block exists.
