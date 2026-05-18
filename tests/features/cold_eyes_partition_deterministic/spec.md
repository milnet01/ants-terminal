# cold-eyes derivePartition deterministic tiebreak

See [`docs/specs/ANTS-1345.md`](../../../docs/specs/ANTS-1345.md).

## Invariants

- **INV-1** Identical-mtime cluster + truncation: the kept set after
  the `kMaxSpecLanes` cutoff is the lex-smallest names; lex-largest
  surplus is dropped.
- **INV-2** Mixed buckets + truncation: newer-bucket lanes always
  survive; older-bucket truncation drops the lex-largest names of
  the older bucket.
- **INV-3** Idempotency: repeated calls on the same fixture return
  identical lane orders.

The post-truncation presentation order is already path-lex ascending
(unchanged by this fix). The fix tightens the *selection set* under
the truncation cutoff so a `touch` against an unrelated file doesn't
swap which specs are reviewed.
