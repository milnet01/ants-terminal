---
paths:
  - "tests/**"
---

## Test harnesses

- **`audit_rule_fixtures`** — `tests/audit_self_test.sh` matches rule regexes
  against `tests/audit_fixtures/<rule>/{bad,good}.*`. Count-based.
- **Feature-conformance** (`tests/features/*`, label `features`) — each subdir
  pairs `spec.md` with a test compiled into a shared bundle
  (`tests/features/README.md`). To add one: write `spec.md` first and surface
  it for sign-off; write `test_<feature>.cpp`; add it to a bundle's `SOURCES`
  (never `add_executable`); verify it fails against pre-fix code.
  **Then build that bundle's target and check `ctest -N -R <name>` moved** —
  building the wrong target passes silently. `build_target_for` names the
  bundle; it is not guessable from the path.
- **Perf** (`tools/perf-report.sh`, label `perf`) — not in the presets or CI.
  See [`docs/qa/perf-harness.md`](../../docs/qa/perf-harness.md).
- **E2E** (`tools/e2e/`, label `e2e`) — `ctest -L e2e`. See
  [`docs/qa/e2e/README.md`](../../docs/qa/e2e/README.md) and
  [`docs/qa/e2e/cases.md`](../../docs/qa/e2e/cases.md).
