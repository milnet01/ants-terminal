# ANTS-1364 — `session_memory` Set/Delete serialize-once refactor

Behavioural tests asserting the post-write byte-accounting invariant
the refactor preserves: `OpResult.totalBytes` exactly equals the
bytes on disk. See `docs/specs/ANTS-1364.md`.

## Invariants exercised

- **INV-2** Byte-identical OpResult contract. `totalBytes` after a
  Set / Delete operation equals
  `QJsonDocument(post_store).toJson(Compact).size()` — which equals
  the on-disk bytes since `saveStore` writes those bytes verbatim.

## Out of scope here

- **INV-1** (serialize-once invariant) — observable only with
  instrumentation the engine deliberately doesn't expose; rides on
  code review of the ~25 LOC diff at
  `sessionmemoryengine.cpp:221–280`.
- Cap-rejection paths — covered by the existing
  `tests/features/session_memory_engine/` lane.
