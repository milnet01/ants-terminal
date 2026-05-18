# Build warning — repo-visibility null-dereference false positive (ANTS-1554)

GCC's `-Wnull-dereference` value-tracking misanalyses the
`QHash::remove → removeImpl → isEmpty (qhash.h:966 "!d || d->size == 0")`
template inline chain when reached from the `QProcess::finished` lambda
inside `MainWindow::refreshRepoVisibility()`. The short-circuit on the
`!d ||` branch is logically safe — QHash uses a shared-null `d` pointer
and `isEmpty()` handles it correctly — but GCC's inter-procedural
analysis through the lambda-via-signal callsite cannot prove the
short-circuit, and emits a false-positive warning at every build.

Per the "flag warnings, don't dismiss them" workflow rule, the fix is
locked in with this spec + regression test rather than ignored as
"pre-existing".

## Surface

- `src/mainwindow.cpp` — the `connect(proc, ...finished, ...)` lambda
  inside `MainWindow::refreshRepoVisibility()` wraps the
  `m_repoVisibilityProbeInFlight.remove(repoRoot)` call in a tightly-
  scoped `#pragma GCC diagnostic push/pop` block that suppresses
  `-Wnull-dereference` for just that one call site.

## Invariants

- **INV-1** `src/mainwindow.cpp` contains the exact token
  `m_repoVisibilityProbeInFlight.remove(repoRoot)` (the lambda still
  removes the in-flight tracker entry on completion).
- **INV-2** The line immediately preceding the `.remove(repoRoot)` call
  is `#  pragma GCC diagnostic ignored "-Wnull-dereference"` (with the
  exact diagnostic identifier).
- **INV-3** A matching `#pragma GCC diagnostic push` precedes the
  ignored line, and a matching `#pragma GCC diagnostic pop` follows
  the `.remove(repoRoot)` call.
- **INV-4** The pragma is guarded behind
  `#if defined(__GNUC__) && !defined(__clang__)` so clang builds do
  not see an unknown-pragma warning.

## Rationale

This test does not invoke the compiler — that would inflate the
feature-test runtime budget significantly. Source-grep is sufficient
because the failure mode is a one-line regression: someone removes the
pragma during refactor, the warning returns, build output gets noisy.
The CI/build pipeline still emits the warning text if a regression
slips through.

If the underlying GCC false positive is fixed upstream, the pragma can
be retired and this test deleted — neither change happens accidentally.
