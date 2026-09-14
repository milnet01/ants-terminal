# Feature spec: ANTS-5067 — in-process drift lanes off the calling thread

The contract is `docs/specs/ANTS-5067-drift-lanes-off-thread.md`. This test
locks its invariants; the numbering below is that spec's.

## Invariants under test

- **INV-1** — `AuditDialog::runNextCheck` runs an `inProcessRunner` on a
  `QThread::create` worker, never on the GUI thread. Source scrape.
- **INV-2** — the dialog discards a lane result from an earlier run: the
  delivery compares a captured generation with `m_runGeneration`, and
  `runAudit` and `cancelAudit` both increment it. Source scrape.
- **INV-3** — `AuditRunner::internal::runInProcessLanes` returns by its
  deadline while a lane still runs, and reports that lane `timed_out` with
  empty output.
- **INV-4** — lanes that finish inside the budget are `ok`, carry their
  output, and come back in input order.
- **INV-5** — an abandoned worker starts no further lane.
- **INV-6** — `budgetMs <= 0` calls no lane.
- **INV-7** — a default sweep with no external tool on `PATH` reports
  `spec_code_drift` with status `ok`. Passes before the change; its red proof
  is the mutation that drops the lanes from the aggregate cap. Needs a fresh
  process (ctest runs each test in its own).
- **INV-8** — `AuditRunner::runAudit` routes each lane outcome's status
  through `finish`. Source scrape.

## Red proof

Before the fix, INV-1, INV-2 and INV-8 fail on the scrape, and INV-3 to
INV-6 fail against the `runInProcessLanes` stub that returns no outcomes.
