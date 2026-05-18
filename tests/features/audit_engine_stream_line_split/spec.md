# Audit-engine streaming line-split

See [`docs/specs/ANTS-1339.md`](../../../docs/specs/ANTS-1339.md).

## Invariants

- **INV-1** `src/auditengine.cpp` does NOT contain `raw.split('\n'`
  inside the `applyFilter` body — the streaming walk replaces it.
- **INV-2** `AuditEngine::applyFilter` against 100 short lines with
  `maxLines=10` returns 10 lines and honours `dropIfContains` +
  `keepOnlyIfContains`.
- **INV-3** Throughput: 5-line bail on a 1 MiB synthetic input
  completes in under 250 ms (proxy for working-set ceiling — full-
  split regression takes seconds and allocates GBs).
- **INV-4** Tail handling: input without trailing `\n` yields the
  final line as a kept candidate.
- **INV-5** Empty input → `{ "", 0 }`.
