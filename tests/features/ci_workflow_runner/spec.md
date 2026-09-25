# The local CI run executes ci.yml, not a copy of it (ANTS-5322)

**Why this exists.** `tools/ci-parity.sh` used to re-state
`.github/workflows/ci.yml`'s steps in shell (ANTS-4392). The two could drift,
and a drifted copy returns green for a pipeline that will fail —
`~/.claude/standards/local-gate.md` § 3. Now `tools/ci_workflow.py` runs each
job's own `run:` steps, and `ci-parity.sh` only picks the jobs.

## Invariants

**INV-1 — every `run:` step of a host job is executed as ci.yml writes it.**
For `build-test`, `build-asan` and `cppcheck`, `ci_workflow.py plan <job>`
lists every `run:` step, and every line of each step's script appears in the
plan unchanged. No step is dropped, reordered or rewritten.

**INV-2 — `ci-parity.sh` carries no copy of a step.** No non-comment line of
it invokes `ctest`, `cppcheck`, `cmake --build`, `appstreamcli`,
`desktop-file-validate`, `groff` or `shellcheck`. Every job id
in ci.yml appears in its `host_jobs` or `container_jobs` list, which is what
its own start-up check enforces at run time.

**INV-3 — a step runs with the runner's semantics.** On a fixture workflow:
the workflow's, the job's and the step's `env` apply, `CCACHE_*` included
(ci.yml points them at its own `.ccache/`, so its cap bounds that cache and not
the developer's); `working-directory` sets the cwd; `CI=true` and `LC_ALL=C.UTF-8` are
set and `DISPLAY` and `BASH_ENV` are not; `timeout <duration> cmd` runs `cmd`; after a failing
step a later step runs only if it has `if: always()`; the run exits non-zero.

**INV-4 — what has no local meaning is refused, not guessed.** Each of these
makes `plan` exit 3: an unknown `uses:` action; an unknown `${{ }}`
expression; an `if:` other than `always()`; a key the runner does not
understand at workflow, job or step level (`defaults`, `strategy`,
`container`, `continue-on-error` and the rest); and a `run:` that reads a
`GITHUB_*` or `RUNNER_*` variable the runner does not set, which includes
writing `$GITHUB_ENV`, `$GITHUB_PATH` or `$GITHUB_OUTPUT`. A refused job fails
`tools/ci-parity.sh`; it is never skipped. A job-level `if:` other than the
one INV-5 names is refused too.

**INV-5 — a job gated off push still runs locally (ANTS-5343).** A job whose
`if:` is exactly `github.event_name != 'push'` is planned and run, and the
plan names the condition as `not on push`. GitHub skips such a job on a push;
locally the caller chose the job by name (the pre-push hook, `ci-parity.sh`),
so the condition has already been decided.

## Not covered

The podman legs (`tools/qt62-guard.sh`) run their own two-line compile inside
the container; only their package list is taken from ci.yml.

The test exits 77 (skipped) when PyYAML is absent. ci.yml installs
`python3-yaml` in the jobs that run the suite so CI does not skip it.
