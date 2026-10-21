# testrunners_detect — feature-conformance contract

**Theme:** `TestRunners::detect` — which runner a project uses, from its
marker files. Full spec: `docs/specs/ANTS-5503-multi-runner-tests.md` § 2.2.

## Invariants

- **INV-1** (the spec's INV-1): `detect` returns the first runner in the
  order ctest, cargo, go, vitest, jest, pytest whose marker exists under the
  root, with that marker as `evidence`, and nothing when none does. A
  `package.json` naming neither jest nor vitest is no marker.

## Out of scope

`focused_test`'s use of the result — `tests/features/mcp_focused_test/`.
