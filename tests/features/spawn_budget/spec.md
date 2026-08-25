# spawn_budget — the budget a test fixture gives a spawned helper (ANTS-4651)

`tests/support/testspawn.h` — `ants_test::spawnTimeoutMs()` and
`ants_test::waitForHelper(QProcess&, QString *why)`. Header-only, Qt6::Core
only, `inline` throughout because several bundles compile it. It is test
support, not product code: nothing under `src/` includes it.

## Why it exists

Twelve fixtures across eight feature directories carried a hard
`p.waitForFinished(5000)` around a `git` invocation. On 2026-08-25, CI run
32819126506 failed `build-asan` on two of them:

| test | job | result |
|---|---|---|
| `VerifyChangesBuildCache.ReentrancyRejectedWhenFlagSet` | build-asan | Failed, 5.79 s |
| `VerifyChangesBuildCache.HitWithinTtl` | build-asan | Failed, 5.58 s |
| both of the above | build-test (Release) | Passed, 0.04 s each |

Both failed inside FIXTURE SETUP — `initGitProject` — not in an assertion
about the code under test, and the ten CI runs before it were green. `git` is
a separate, unsanitised process, so what the sanitizer job costs here is
contention rather than git's own runtime; the number had been calibrated on a
quiet Release box.

**That is the defect `CMakeLists.txt` already names one layer up**, where
ctest's per-test `TIMEOUT` went from 10 s to 60 s on 2026-07-31 for the same
reason: *a HANG detector, not a performance budget, calibrated against Release
only*. A five-second wait for a subprocess is the same mistake inside the test.

## Contract

- **INV-1** — one named budget, generous by default and overridable by
  `ANTS_TEST_SPAWN_TIMEOUT_MS`. The default clears the measured 5.79 s failure
  by roughly 4× and stays well inside the bundles' 60 s ctest `TIMEOUT`, which
  is the layer that terminates a genuine hang; racing it would mean the outer
  killer reports the hang, with a worse message than the inner one gives.
  *Test:* `SpawnBudget.Inv1DefaultIsGenerousAndOverridable`.
- **INV-2** — an unparseable or out-of-range override falls back to the
  default, never to 0. *Test:* `SpawnBudget.Inv2MalformedOverrideFallsBackNotToZero`.
- **INV-3** — a helper that finishes is not waited out; the budget is a
  ceiling, not a sleep. *Test:* `SpawnBudget.Inv3FastHelperReturnsPromptly`.
- **INV-4** — an overrunning helper is killed, and the diagnosis names the
  command, the elapsed time and the budget.
  *Test:* `SpawnBudget.Inv4OverrunIsKilledAndDiagnosed`.
- **INV-5** — "could not be started" and "did not finish" are reported
  differently, and the former consumes no budget.
  *Test:* `SpawnBudget.Inv5FailedToStartIsNotATimeout`.

## Trap cases

| INV | What passes without the fixture |
|---|---|
| 1 | a default raised without an upper bound, which then races the 60 s ctest TIMEOUT and hands the report to the outer killer |
| 2 | `QByteArray::toInt()` returning 0 on junk, producing a budget that fails every fixture instantly and reads as the code being broken |
| 3 | an implementation that sleeps out the whole budget before checking, which passes every boolean assertion and makes the suite unusably slow |
| 4 | returning false without killing — the helper survives the test, and on a bundle that is the next test's contention |
| 5 | one boolean for both causes. This is the half that cost the diagnosis: the repairs are opposite — install the tool, versus give it more room — and the old message named neither the command nor the budget |

## Not covered here

**The twelve converted call sites are covered by their own suites**, which must
stay green: `verify_changes_build_cache`, `mcp_roadmap_branch_drift`,
`roadmap_viewer_archive`, `audit_scope_flat_layout`, `audit_run_since_last_run`,
`roadmap_last_touch_async`, `mcp_orientation_install`, `debt_sweep_engine`.

**Other spawn budgets in the tree are NOT converted** — 2000, 3000, 10000,
15000 and 30000 ms literals remain in other fixtures. They are out of
ANTS-4651's scope, which is the 5000 ms class that reddened CI; the helper is
now available to them, and converting one is a one-line change.

## Test

`tests/features/spawn_budget/test_spawn_budget.cpp`, compiled into the
`test_core` bundle (Core-only) per `tests/features/README.md` — not a
standalone `add_executable`. Verified RED against a stubbed header before the
implementation landed: all five failed on assertions, none on compile.
