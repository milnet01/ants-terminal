# Feature: check-roadmap — `--allow` exempts exactly the ids it names

Test contract for ANTS-6656. `tools/check-roadmap.sh --allow ID,ID` exempts
the listed ids from its duplicate-id check.

## Invariants under test

- **INV-1** — a `+` in an allowed id is literal: `--allow ANTS-1+` does not
  exempt `ANTS-11`.
- **INV-2** — a `.` in an allowed id is literal: `--allow ANTS-1.` does not
  exempt `ANTS-11`.
- **INV-3** — an unbalanced `(` in an allowed id exits 1 and reports the
  duplicates, not exit 2 with an awk regex error.
- **INV-4** — the ids the list names are exempt and no others; both named
  exits 0, and an empty list exempts nothing.

## Test plan

`test_check_roadmap_allow.sh` runs the real script against a throwaway
roadmap holding two duplicated ids, `ANTS-11` and `ANTS-2`. Each run is
capped at 10 s. Red before ANTS-6656 on INV-1 to INV-3: the list was built
into a regex, its escaping missed `(`, `)`, `+`, `?`, `{` and `|`, and
`awk -v` stripped the backslashes it did add.

The suite exits 77 (skipped) when awk is missing.

Label: `features;fast`.
