# testrunners_parse — feature-conformance contract

**Theme:** `TestRunners::sniff` and `TestRunners::parse` — recognising and
parsing the output of pytest, jest, vitest, cargo and go. Full spec:
`docs/specs/ANTS-5503-multi-runner-tests.md` § 2.3.

Every fixture under `fixtures/` is a real run's output; `fixtures/VERSIONS.md`
names the runner versions and commands.

## Invariants

The spec's INV-2, INV-3, INV-4, INV-5 and INV-7:

- **INV-2**: `sniff` returns ctest for a ctest log, even one carrying a
  pytest summary line.
- **INV-3**: for each runner, `parse` yields the passed, failed and skipped
  counts and failing names the run printed, and `total` as their sum.
- **INV-4**: pytest errors count as failed, `xfailed` as skipped, `xpassed`
  as passed; cargo `ignored` as skipped, summed over every `test result:`.
- **INV-5**: go counts top-level tests only, and output without `=== RUN `
  is not recognised.
- **INV-7**: `collected` is set only from pytest's `collected N items`.

Also locked: a go timeout or panic counts as a failure, and jest's and
vitest's `todo` count as skipped.

## Out of scope

The verbs around these functions — `tests/features/mcp_test_results/` and
`tests/features/mcp_focused_test/`.
