# indie_review_corroborate truncation flag

See [`docs/specs/ANTS-1344.md`](../../../docs/specs/ANTS-1344.md).

## Invariants

- **INV-1** `IndieReviewEngine::kMaxScanBytes` is declared as
  `constexpr int` equal to 64 * 1024 in `indiereviewengine.h`.
- **INV-2** `cmdIndieReviewCorroborate` populates `truncated_lanes` /
  `truncated` / `truncated_at_bytes` on the envelope when a report
  exceeds `kMaxScanBytes`. Source-grep against `remotecontrol.cpp`.
- **INV-3** `cmdCrossDocDiff` carries the same envelope keys (parity).
- **INV-4** The implementation gates envelope emission on
  `!truncatedLanes.isEmpty()` so the v1 envelope shape is preserved
  when no truncation occurred.
