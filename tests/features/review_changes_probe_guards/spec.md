# Feature: Review Changes probe bounds and single finalize

## Invariants

**INV-1 — the cross-branch unpushed log is bounded.** The
`log --branches --not --remotes` probe in `src/diffviewer.cpp` passes
`--max-count`, so a repository with no remote-tracking refs does not list its
whole history on every refresh.

**INV-2 — a probe finalizes once.** The probe's `errorOccurred` handler
finalizes only on `QProcess::FailedToStart`, the one error after which
`finished` is not emitted.

## Rationale

The ANTS-5109 performance pass found both: with no remotes, `--not --remotes`
excludes nothing, and for a crashed or timed-out git both handlers called
`finalize()`.

## Test surface

`test_review_changes_probe_guards.cpp` reads `src/diffviewer.cpp` (located from
the test's own path).

## Regression history

- **ANTS-5109:** the two defects above. Locked by this spec.
