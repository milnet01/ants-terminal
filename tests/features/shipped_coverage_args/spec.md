# Feature: check-shipped-coverage — its inputs reach SQL safely

Test contract for ANTS-6631. `tools/check-shipped-coverage.sh` pastes its
`--since` date and the checkout path into SQL text.

## Invariants under test

- **INV-1** — `--since` with no value exits 2 with a usage message. Before,
  `shift 2` failed with one argument left and the loop never ended.
- **INV-2** — a `--since` value that is not `YYYY-MM-DD` exits 2 before any
  query runs.
- **INV-3** — a checkout path holding a `'` still finds its project in the
  store, so the gate checks the CHANGELOG instead of skipping with exit 0.
  A control case shows the same fixture passes once the CHANGELOG cites the
  item.

## Test plan

`test_shipped_coverage_args.sh` runs the real script in a throwaway git repo
named `it's repo`, against a throwaway store under `XDG_DATA_HOME` holding
the two tables the script reads. Each run is capped at 10 s, so the old
endless loop fails as exit 124 rather than hanging. Red before ANTS-6631 on
INV-1 to INV-3.

The suite exits 77 (skipped) when git or sqlite3 is missing.

Label: `features;fast`.
