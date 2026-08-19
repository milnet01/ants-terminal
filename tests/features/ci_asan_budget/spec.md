# CI's sanitizer job stays inside its budget, and says so in red when it does not

**Why this exists.** Measured 2026-08-19 (ANTS-4533): `ci.yml`'s `build-asan`
job was cancelled at its 30-minute cap on 36 of the last 40 runs, going back
at least two days. It went unnoticed because **a job killed by
`timeout-minutes` concludes `cancelled`, not `failure`** — the run does not
read as red, and no notification fires. The pre-push hook's ASan leg is
cost-gated (ANTS-4118) and skips on a cold tree, so local and remote could
both decline the sanitizer suite on the same push with nothing saying the
coverage was zero. That is what happened to the ANTS-4505/4506 push.

Three measurements decided the remedy. Step timings from the last four runs
where `build-asan` completed, against the run that did not:

| run | Build | sanitized ctest |
|---|---|---|
| 32224813130 (success) | 11m38s | 16m17s |
| 32245859518 (success) | 11m29s | 15m22s |
| 32280374403 (cancelled) | 16m42s | >13m16s, killed |

1. **The suite has not outgrown 30 minutes — it sits exactly on it.** A
   completing run spent 27–28 minutes against a 30-minute cap, so cache
   warmth alone decided the outcome. Raising the cap on its own would buy
   one release cycle and hide the next breach just as well.
2. **A cancelled job never saves its ccache.** `actions/cache`'s implicit
   post-step is skipped when the job is cancelled — `Post Restore ccache
   (asan)` reads `skipped` on every timed-out run. So the cache stays frozen
   at the last *completing* run while `main` moves on, every later build
   starts colder, and the timeout becomes more likely. That is a
   self-reinforcing spiral, and it is why the cancelled run's Build took
   16m42s against 11m29s six hours earlier.
3. **The sanitized suite is serial.** `Total Test time (real) = 922.23 sec`
   for 3650 tests on a 4-vCPU runner, dominated by per-process startup under
   ASan rather than by any one test (the two slowest total 195s). The
   pre-push hook has run this suite at `-j2` with a per-test `--timeout 300`
   since ANTS-3761; `ci.yml` never picked either up.

## The invariants

**INV-1 — the sanitized `ctest` runs in parallel and caps each test.** The
`build-asan` job's `ctest` invocation carries `-j` and `--timeout`. Parallelism
is the only lever that matters at this test count; the per-test cap converts
"one hung test eats the whole budget" into one failed test.

**INV-2 — a step that overruns exits non-zero.** The `build-asan` job's Build
and ctest commands are wrapped in `timeout`, so an overrun exits 124 and the
step FAILS. This is the invariant the whole item turns on: a red run is seen,
a cancelled one is not.

**INV-3 — the ASan ccache is saved even when the job does not complete.** An
`actions/cache/save` step guarded by `if: always()` runs after the build, so a
later overrun cannot rob the next run of its cache and deepen the spiral.

**INV-4 — the per-step budgets sum below the job cap.** If they did not, the
job-level `timeout-minutes` would fire first and the run would read
`cancelled` again, silently undoing INV-2. This is the invariant that keeps
INV-2 true after someone raises a number.

## What this check does NOT cover, stated so it is not mistaken for coverage

- **It does not verify the budgets are big enough.** INV-4 checks ordering,
  not sufficiency. A build that legitimately grows past its `timeout` will go
  red — which is the designed outcome, and the signal to re-measure and raise
  both numbers together.
- **It does not verify `-j2` is the right width, or that it is safe.** The
  number is inherited from the pre-push hook and from the `workstation`
  preset's caution, not derived here.
- **It does not parse YAML.** It slices the `build-asan` job by its heading
  and scrapes that slice. A restructuring that renames the job defeats it —
  which surfaces as this test failing, not as a silent pass.
- **It asserts nothing about `build-test` or `qt62-baseline`.** Both complete
  inside their caps today; neither is in scope.

## Build

Compiled into the **`test_claude`** bundle, which already carries the
`SRC_CI_WORKFLOW_PATH` compile definition for `ci_workflow_deps`. Label
`features;fast`.
