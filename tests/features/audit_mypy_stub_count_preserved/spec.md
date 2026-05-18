# Mypy stub-hint consolidation — pre-collapse count preserved

See [`docs/specs/ANTS-1343.md`](../../../docs/specs/ANTS-1343.md).

## Invariants

- **INV-1** `CheckResult` declares `bool findingCountAuthored = false;`
  (default-false) so existing callers see v1 arithmetic.
- **INV-2** Non-mypy `CheckResult`: consolidator is a no-op
  (`findingCountAuthored` stays false).
- **INV-3** Mypy `CheckResult` with a single stub package: consolidator
  declines to fold (the dispatcher's v1 arithmetic still applies).
- **INV-4** Mypy `CheckResult` with N ≥ 2 distinct stub packages:
  - consolidator stamps `findingCountAuthored = true`,
  - `findingCount == preCollapseN + omittedCount`,
  - `findings.size()` shrinks to 1 (the synthetic Info hint).
- **INV-5** Dispatcher source-grep at `auditdialog.cpp`: the post-cap
  line gates on `!r.findingCountAuthored`.
