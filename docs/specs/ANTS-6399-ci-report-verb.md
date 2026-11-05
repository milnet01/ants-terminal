# ANTS-6399 — `ci_report`, a verb that reads a project's CI run history and warns about flakes, budgets and speed

**Status:** accepted (2026-11-05).
**Kind:** feature.
**Source:** ROADMAP ANTS-6399 (user-request-2026-11-05; scope agreed with the claude-config session the same day).
**Composes with:** `~/.claude/tools/ci-gate`, which owns the workflow-file checks this verb defers (§ 3).

**Layman:** A tool every Claude session can ask: is this project's CI flaky, close to timing out, or slower than it needs to be?

## 1. Goal

A session in any registered project with GitHub Actions asks one verb and
gets a list of CI problems. Each one names the runs and commits that show
it and the standard that says what to do. When the last saved report holds
a real problem, `session_orient` says so in one line, without contacting
GitHub.

## 2. Problem

1. **No verb reads CI run history.** No MCP verb runs `gh` or calls the
   GitHub API (`workspace_search` for `"gh"` over `src/`: the only caller
   is `MainWindow::refreshRepoVisibility`, which is terminal code, not a
   verb). A session learns about a flaky test or a step near its timeout
   only when a run goes red.
2. **The budget overruns this project hit were found by hand.** ANTS-5528
   (the nightly `build-asan` cache evicted, its cold build overran its
   guard) and ANTS-6398 (the ASan test step's budget raised to 38 minutes
   after measuring step times by hand) were both visible in run history
   before anyone looked.
3. **`ci-gate` reads workflow files, not runs.** It reports a job with no
   `timeout-minutes` and a workflow with no `concurrency` or no cache
   (`local-gate.md` § 9, last paragraph). How close a job runs to its
   budget, whether a test fails then passes on one commit, and how full the
   cache is are facts only the run history holds.

## 3. Scope decisions (agreed with the user)

- **An Ants MCP verb shipped with Ants Terminal and served by `ants-mcpd`**;
  on demand, plus one `session_orient` line when a project's CI shows a
  real problem. User, 2026-11-05.
- **The `session_orient` line reads the last saved report and never
  contacts GitHub.** The warning can be old; the reply says how old. User,
  2026-11-05.
- **GitHub Actions only in this version.** A project with no
  `.github/workflows` or no GitHub remote gets `ci: "none"` with the
  reason. Local push-gate timing is deferred to ANTS-6401. User,
  2026-11-05.
- **One owner per check** (claude-config, 2026-11-05). This verb owns
  run-history findings. It defers: a job with no `timeout-minutes`, a
  workflow with no `concurrency` or no cache → `ci-gate`; unpinned `uses:`
  and `permissions` → zizmor and `check-code`; behind or deprecated action
  versions and images → `check-dependencies`.
- **Every finding names its run ids and commit SHAs. It never suggests a
  push or a re-run** (`commits.md` § 4.1). A flake's fix is in the test,
  never a retry. claude-config, 2026-11-05.
- **Budgets are judged against a cold build** (`local-gate.md` § 9,
  `timeout-minutes` row). A finding never advises lowering a budget.
- **Parallelism advice is for tests only**, and only where tests are
  isolated and each has a timeout. Build `-j` is sized from memory, not
  vCPUs, so the verb never advises it. claude-config, 2026-11-05.
- **Dropped from the agreed list: "a test step with wall time far above
  CPU time".** GitHub's jobs API reports step start and end times only,
  with no CPU time (probed 2026-11-05 with
  `gh api repos/{owner}/{repo}/actions/runs/41998362846/jobs`: each step
  carries `name`, `number`, `started_at`, `completed_at`, `conclusion`).
  Author's call; reported to claude-config with this draft.
- **Retiring runner images come from GitHub's own annotations**, not from a
  list this verb keeps. GitHub posts a warning annotation on runs that use
  an image it is retiring. A list kept here would go stale. Author's call.

## 4. Surface

### 4.1 Files

| File | Holds |
|---|---|
| `src/cireport.h`, `src/cireport.cpp` | `CiReport::analyse()`: pure. Takes the fetched history, the workflow budgets and `now`; returns the findings. No I/O. |
| `src/cihistory.h`, `src/cihistory.cpp` | `CiHistory::fetch()`: runs `gh` and reads the cache file. All network and process work. |
| `src/ciworkflows.h`, `src/ciworkflows.cpp` | `CiWorkflows::read()`: the line reader of § 4.4. Reads files, no network. |
| `src/remotecontrol_ci.cpp` | `RemoteControl::cmdCiReport`, the verb body; and `buildCiReportBlock` for `session_orient`. |

All four join the library the other project-scoped verbs link
(`dependency_inventory`'s files are the model). The verb is registered in
`mcp::registerProjectScopedVerbs()` (`src/mcptoolregistry.cpp`) with
`CallerCwdContract::Required` on `DispatchLane::Bulk`, because a live call
holds its worker for up to 120 s (§ 4.5). It is listed in
`callerCwdContractFor`, in `rateLimitClassFor` as `Expensive`, and in the
verb-family list in `src/claudeintegration.cpp`. Its schema sits beside
`dependency_inventory`'s in `ClaudeIntegration::handleMcpRequest`.

### 4.2 The verb

```
ci_report {caller_cwd, source?: "live"|"saved", days?: 1..90, workflow?: string}
```

- `source:"live"` (default) fetches history, analyses it, writes the saved
  report where § 4.5 says so, and returns it. `source:"saved"` returns the saved report
  with no network and no `gh` call.
- `days` (default 30) bounds the window by run creation time.
- `workflow` keeps runs whose workflow name equals it exactly.

Reply:

```json
{
  "ok": true,
  "ci": "github",
  "repo": "owner/name",
  "generated_at": "2026-11-05T09:10:00Z",
  "window": {"days": 30, "runs": 41, "workflows": ["CI"]},
  "truncated": false,
  "truncated_reason": null,
  "findings": [
    {
      "check": "near_budget",
      "severity": "problem",
      "workflow": "CI", "job": "Build and test (ASan/UBSan)", "step": null,
      "message": "Job ran 34m against a 38m budget (89%) in 3 of the last 10 runs.",
      "runs": [41993542921], "shas": ["c0de1b2d…"],
      "cache": "miss",
      "cites": ["local-gate.md § 9 (timeout-minutes row)"]
    }
  ],
  "counts": {"problem": 1, "advice": 0}
}
```

`ci` is `"github"` or `"none"`. With `"none"` the reply carries
`ci_none_reason` (`"no_workflows"` or `"no_github_remote"`), an empty
`findings`, and writes no saved report. Both reasons are decided before any
`gh` call: `no_github_remote` when no git remote's URL names `github.com`. `cache` is `"hit"`, `"miss"` or
`"unknown"` and is present on timing findings only (§ 4.3). `runs` and
`shas` hold at most 10 entries each, newest first. `findings` is sorted
by severity (`problem` first), then `check`, then `workflow`, `job`,
`step`.

### 4.3 The checks

Severity `problem` is a check that predicts a red run. `advice` is a speed
or hygiene change.

| `check` | Severity | Fires when | Cites |
|---|---|---|---|
| `flaky` | problem | One job, in one workflow, event and SHA, fails in an attempt or run and passes in a later one. The event keeps a pull request's run apart from a push run of the same head SHA, which tests different code. Names the failed job and step; adds failing ctest names when the failed job's log carries ctest's `The following tests FAILED:` block. | `testing.md` § 7; `local-gate.md` § 9 rule 3 |
| `near_budget` | problem | A job or step with a budget (§ 4.4: `timeout-minutes`, or a step's shell guard) ran at 80% of it or more in any run in the window, or ended at its budget. | `local-gate.md` § 9, `timeout-minutes` row |
| `budget_trend` | advice | A budgeted job's median time over its newest 5 successful runs is 25% or more above the median of its earlier ones in the window, with at least 5 earlier ones. | `local-gate.md` § 9, `timeout-minutes` row |
| `scheduled_red` | problem | A workflow's newest `schedule` run failed. The message says since when it has been red: the newest successful scheduled run's date, or "no green run in the window". | `local-gate.md` § 9, slow-legs row |
| `queue_wait` | advice | A job's median wait from creation to start exceeds 5 minutes. | none |
| `annotation` | advice | A warning annotation on the newest run of a workflow mentions deprecation or a runner image being retired (case-insensitive `deprecat`, `retire`, `will be removed`, `brownout`). Quotes the annotation. | none |
| `runner_latest` | advice | A job's `runs-on` is a `*-latest` label. | `local-gate.md` § 9, warm-containers row, condition 1 |
| `cache_size` | advice | The repository's Actions cache usage is 80% or more of 10 GB. | `local-gate.md` § 9, caches row (prune) |
| `cache_key_shared` | advice | Two jobs save `actions/cache` keys with the same prefix: the key with its last `${{ … }}` and everything after it removed, compared as text (`${{ runner.os }}-ccache-release-`). | `local-gate.md` § 9, caches row |
| `cache_save_skipped` | advice | An `actions/cache/save` step's `if:` lacks `always()`, or a job uses the combined `actions/cache`, whose save does not run when the job fails. | `local-gate.md` § 9, caches row |
| `test_parallelism` | advice | A test step's command runs `ctest` with no `-j`/`--parallel` and no `CTEST_PARALLEL_LEVEL` in the workflow's, job's or step's `env`, and the same step carries `--timeout`. The message states the isolation precondition. | `local-gate.md` § 9, test-parallelism row |

**Cache hit or miss beside timing findings.** For each `near_budget` and
`budget_trend` finding, the verb reads the log of the newest run's job
behind it and reports `hit` on `Cache restored from key`, `miss` on
`Cache not found for input keys`, `unknown` otherwise.

`test_parallelism` fires only where the step already sets a per-test
timeout, because isolation cannot be read from a workflow file and a
missing timeout is the half that can.

### 4.4 Reading budgets from workflow files

`CiWorkflows::read(root)` reads `<root>/.github/workflows/*.yml` and
`*.yaml` from the working tree, line by line. It recognises, by
indentation: the workflow's `name:` and `env:` keys, the `jobs:` map, each
job id, and in a job `name:`,
`runs-on:`, `timeout-minutes:`, `env:` keys, and `steps:` entries with
their `name:`, `uses:`, `run:` (single line or `|` block), `if:`,
`with: key:`, `env:` keys and `timeout-minutes:`.

- A run's `workflowName` is matched to a file by the file's top-level
  `name:`, else its path, which GitHub shows for an unnamed workflow. Its
  jobs are matched only within that file.
- A job is matched to the API's job name by its `name:`, else its id,
  exactly. Where none matches exactly, an API name beginning `<name> (`
  matches the job named `<name>`: a matrix job's API name is
  `<name> (<values>)`. That fallback applies only when exactly one job
  fits and no job's `name:` holds an expression.
- **A value it cannot read is unknown, never a guess.** A `timeout-minutes`
  holding an expression (`${{ … }}`), a job it cannot match, or a file it
  cannot parse leaves that budget unknown. A check needing an unknown value
  does not fire. The reply's `unread` array lists each `{file, job, field}`
  it skipped; a workflow no file matches has `file` null and names itself
  in `workflow`.
- **An alias or a merge key (`<<: *x`) makes the file unread**, because the
  keys it brings would be lost unnamed. One-line flow maps and lists
  (`with: {key: …}`) are read, and a block-map `runs-on:` names its runner
  in `labels:`.
- **A shell guard is a step budget.** A step whose single-line `run:`
  begins `timeout <N><s|m|h> ` has that duration as its budget, as
  `run: timeout 34m cmake --build build-asan --parallel` in this project's
  `ci.yml` does. Where the step also has `timeout-minutes`, the smaller
  counts. A guard anywhere else in the command (after `&&`, in a `|`
  block, inside a script) is not read. Such a step that ran its full
  budget fails rather than being cancelled, and counts as reaching it.

The working tree is read, not each run's commit. Budgets are judged against
today's file, which is what a fix changes.

### 4.5 Fetching, limits and the saved report

`CiHistory::fetch()` runs `gh` through `HostExec`, so a Flatpak build runs
the host's `gh`.

- **Calls, in order:** `gh repo view --json nameWithOwner,defaultBranchRef`;
  `gh run list --json databaseId,headSha,headBranch,conclusion,event,workflowName,createdAt,attempt,status --limit 200`
  (runs older than the window are dropped);
  `gh api repos/{repo}/actions/runs/{id}/attempts/{n}/jobs` per run and per
  attempt, newest run first, every page or none of that attempt; `gh api repos/{repo}/actions/cache/usage`;
  `gh api repos/{repo}/check-runs/{job}/annotations` for the jobs of each
  workflow's newest run; `gh api repos/{repo}/actions/jobs/{job}/logs` for
  logs (§ 4.3), at most 10 logs, keeping the first and last MiB of each:
  the cache line is near a log's start and ctest's failure list at its end.
- **Limits.** Each call times out at 20 s. The whole fetch stops at 120 s
  or 300 calls, whichever comes first. Stopping sets `truncated:true`,
  `truncated_reason` (`"deadline"` or `"call_cap"`), and the analysis runs
  on what was fetched. When the oldest of 200 listed runs is still inside
  the window, older runs are not seen, and the reply sets `truncated:true`
  with `truncated_reason:"run_limit"`. The jobs calls stop when 30 calls or
  30 s of the limits remain, so the calls after them still run.
- **One live run per project at a time.** A live run holds an exclusive
  lock on a file beside its saved report, and writes its start time there
  for `already_running`'s `running_since_ms`; `retry_after_ms` is the time
  left of that run's 120 s. Each Claude session runs its own
  `ants-mcpd`, and one process runs its Bulk calls one after another, so
  two overlapping runs are in two processes. A second `source:"live"`
  call for the same root refuses `already_running` while the lock is held.
- **The saved report** is the reply, written atomically to
  `~/.cache/ants-terminal/ci-report/<cwdHash(canonical root)>.json`
  (`SessionMemoryEngine::cwdHash`, as the other per-project caches), mode
  0600, after every live run that reached GitHub with no `workflow`, the
  default `days` and an empty `fetch_errors`. The root, for the saved
  report and for `CiWorkflows::read`, is `resolveRootCanonical(m_roots, req)`
  on both the verb and `buildCiReportBlock`, as `session_orient` resolves it. A row joins
  `docs/standards/mcp-caches.md` § Cache inventory.

### 4.6 The `session_orient` line

`buildCiReportBlock(root, now)` reads the saved report and adds a
`ci_report` key to `session_orient`'s reply only when the report holds one
or more `problem` findings:

```json
"ci_report": {"problems": 2, "first": "flaky: CI / Build and test (Release), 2 runs", "generated_at": "2026-11-05T09:10:00Z", "age_days": 3, "hint": "ci_report {source:\"saved\"} for the list"}
```

It runs no process and reads one file. A missing or unreadable file adds
nothing. `age_days` is the count of UTC calendar days from `generated_at`'s date
to `now`'s date, so the
block, and orient's ETag with it, changes only when the file or the day
changes.

### 4.7 Refusals

| Code | When | Caller's next step |
|---|---|---|
| `no_project` | `caller_cwd` does not resolve to a directory (`ants::requiredCallerCwd`) | pass the project's path |
| `runner_missing` | `gh` did not start (outside a Flatpak) | install `gh` |
| `github_unavailable` (new) | `gh repo view` or `gh run list` exited non-zero: not signed in, offline, rate-limited, or, inside a Flatpak, not installed on the host (`flatpak-spawn` starts either way). Carries `stderr_tail`, the last 2 000 characters | `gh auth status`, or wait |
| `already_running` | § 4.5 | wait |
| `not_cached` | `source:"saved"` and no saved report | run `source:"live"` |
| `bad_args` | `days` outside 1..90, or an unknown `source` | fix the call |

A later call failing (one run's jobs, a log, the annotations) does not
refuse. The finding that needed it does not fire, and the reply's
`fetch_errors` lists `{call, exit_code}` for each.

`github_unavailable` joins `docs/standards/mcp-error-codes.md`. The caller's
next step differs from `runner_missing`'s (sign in or wait, against
install), which is that file's test for a new code.

## 5. Invariants

- **INV-1** — `CiReport::analyse()` reports `flaky` for a workflow and SHA
  with a failed attempt followed by a successful one, naming its run ids
  and the failed job, and reports nothing for a SHA whose runs all failed
  or all passed. *Test:* `tests/features/mcp_ci_report`, fixture
  `flaky_attempts.json`. *Breaks when:* the check groups by SHA alone
  across workflows, so a failed docs workflow and a green CI on one SHA
  read as a flake; the fixture holds that case and expects no finding.
  Amended by ANTS-6402: it also breaks when a later green run in which
  the job was skipped, or a run for another event, reads as the job
  passing. *Test:* `McpCiReport.Ants6402FlakyNeedsTheSameJobToPass`.
- **INV-2** — `near_budget` fires at 80% of a job's `timeout-minutes` and
  not below it, and fires for a job `cancelled` at its budget. *Test:*
  `tests/features/mcp_ci_report`, fixtures at 79% and 80% of a 10-minute
  budget, and one cancelled at 10m00s. *Breaks when:* the ratio uses the
  run's span rather than the job's, so queue time counts as run time; the
  79% fixture's job starts 3 minutes after its run is created, so a
  run-span ratio reads it as over 100% and fires.
- **INV-3** — a budget `CiWorkflows::read()` cannot read is unknown, no
  check that needs it fires, and `unread` names it. *Test:*
  `tests/features/mcp_ci_report`, a workflow whose `timeout-minutes` is
  `${{ matrix.t }}` and a job running at 99% of 10 minutes. *Breaks when:*
  the reader falls back to a default budget (GitHub's 360 minutes or any
  other), which would produce or hide a finding from a guess.
- **INV-4** — every finding's `runs` and `shas` are non-empty for checks
  read from run history (`flaky`, `near_budget`, `budget_trend`,
  `scheduled_red`, `queue_wait`, `annotation`), and hold at most 10
  entries. *Test:* `tests/features/mcp_ci_report`, every fixture's
  findings walked. *Breaks when:* a check builds its message from an
  aggregate and drops the run list.
- **INV-5** — no finding's `message` advises a push, a re-run, a retry, or
  a lower budget. *Test:* `tests/features/mcp_ci_report` asserts no
  message across all fixtures, with the names and annotation text it quotes
  from the data masked, matches `(?i)\b(re-?run|retry|push|lower|reduce)\b`.
  *Breaks when:* a `flaky` message suggests re-running the job.
- **INV-6** — `source:"saved"` and `buildCiReportBlock` start no process.
  *Test:* `tests/features/mcp_ci_report` puts a fake `gh` first on `PATH`
  that appends to a log file, calls both, and expects the log absent.
  *Breaks when:* the saved path refreshes a stale report on its own.
- **INV-7** — `session_orient` carries `ci_report` exactly when the saved
  report holds a `problem` finding, and two calls on one UTC day over an
  unchanged file return the same ETag. *Test:* `tests/features/mcp_ci_report`,
  three saved files: none, advice only, one problem. *Breaks when:* the
  block carries a value that moves between calls (a time to the second).
- **INV-8** — the fetch stops at the deadline or the call cap, returns
  `ok:true` with `truncated:true` and the reason, and still analyses what it
  fetched. *Test:* `tests/features/mcp_ci_report`, a fake `gh` listing more
  runs than the call cap allows (the cap is a parameter of `fetch()`,
  lowered in the test). *Breaks when:* hitting the cap refuses or returns
  empty findings.
- **INV-9** — `gh` exiting non-zero on `run list` refuses
  `github_unavailable` with `stderr_tail`, and no `gh` on `PATH` refuses
  `runner_missing`. Neither writes the saved report. *Test:*
  `tests/features/mcp_ci_report`, fake `gh` with `FAKE_FAIL=list`, then a
  `PATH` with no `gh` on it. *Breaks when:* a failed fetch saves an empty report,
  which would clear a real warning from `session_orient`.
- **INV-10** — a project with no `.github/workflows` returns `ok:true`,
  `ci:"none"`, `ci_none_reason:"no_workflows"`, and starts no process.
  *Test:* `tests/features/mcp_ci_report`, an empty temp project with the
  logging fake `gh`. *Breaks when:* the verb calls `gh` before looking for
  workflow files.
- **INV-11** — the verb's body reaches no `MainWindow`, and `ants-mcpd`
  serves it. *Test:* `tests/features/standalone_mcp_server` INV-2 (every
  name `registerProjectScopedVerbs` registers is outside
  `terminalScopedVerbNames()`), plus a source check in
  `tests/features/mcp_ci_report` that the registration sits in
  `registerProjectScopedVerbs` and `cmdCiReport`'s body holds no `m_main`.
  *Breaks when:* `cmdCiReport` uses tab
  state, or is registered among the terminal-scoped verbs.
- **INV-12** — a step whose `run:` begins `timeout 10m ` has a 10-minute
  budget: `near_budget` fires when it ran 8 minutes, and when it failed at
  10 minutes. A step running `cd build && timeout 10m ctest` for 9 minutes
  gets no budget and no finding. *Test:* `tests/features/mcp_ci_report`,
  fixtures with neither step carrying `timeout-minutes` and the job's
  budget at 60 minutes, so only the shell guard can make the finding.
  *Breaks when:* the reader ignores shell guards (the first two fixtures
  stay silent), or matches `timeout` anywhere in the command (the third
  fires).

## 6. Failure modes

| Failure | Effect |
|---|---|
| `gh` signed out or offline | `github_unavailable`; saved report kept |
| Rate limit mid-fetch | later calls fail; `fetch_errors` lists them; findings needing them do not fire |
| Workflow file edited since the runs | budgets judged against today's file (§ 4.4) |
| A job renamed within the window | its older runs do not match by name; trend sees fewer runs and may not fire |
| GitHub changes its log line for cache hits | `cache` reads `unknown`; nothing else changes |
| Saved report written by an older build | `buildCiReportBlock` reads `counts.problem` and the first finding only; a missing key adds nothing |

## 7. Tests

`tests/features/mcp_ci_report/`, label `features;fast`, bundle from
`build_target_for`. Fixtures are recorded `gh` JSON trimmed to the fields
read. The analyser tests feed `CiReport::analyse()` directly. The fetch
and verb tests put a fake `gh` first on `PATH`, as
`tests/features/ci_cache_prune` does. No test touches the network
(`testing.md` § 7). Each test is run against a build with its rule
removed and seen to fail before it lands.

| Test | Covers |
|---|---|
| analyser tests on fixtures | INV-1, INV-2, INV-3, INV-4, INV-5, INV-12 |
| ANTS-6402 tests (`McpCiReport.Ants6402*`) | INV-1's amendment; § 4.4's matrix, alias, flow-map and block-map rules; § 4.5's pages and log ends |
| fake-`gh` verb tests | INV-6, INV-8, INV-9, INV-10 |
| `session_orient` block test | INV-7 |
| source checks, with `tests/features/standalone_mcp_server` | INV-11 |

**Manual check after shipping:** `ci_report` on this project, then on one
project without workflows and one signed out of `gh`.

## 8. Alternatives considered (and rejected)

- **A job id and a poll, as `audit_run {async:true}` does.** Rejected: the
  fetch is bounded at 120 s, the verb is called rarely, and a job registry
  adds state that dies with `ants-mcpd`.
- **Orient fetches live.** Rejected by the user (§ 3): every session start
  would wait on GitHub.
- **A YAML library.** Rejected: one new dependency for the handful of keys
  § 4.4 reads. The reader reports what it skips (INV-3), so a missed key
  costs a finding, never a wrong one.
- **Thresholds in a data file re-read on each call.** Rejected: the
  thresholds are a few constants beside their checks, and INV-2 tests
  `near_budget`'s at its edge.
- **Calling the REST API directly with Qt networking.** Rejected: it would
  need its own token handling. `gh` already holds the user's sign-in.

## 9. Out of scope

- **Local push-gate timing** — deferred to ANTS-6401.
- **Other CI systems** (GitLab, Forgejo) — permanent exclusion: the user
  scoped this verb to GitHub Actions (§ 3). A project that adopts another
  system files its own item.
- **The workflow-file checks `ci-gate`, zizmor and `check-dependencies`
  own** — permanent exclusion: one owner per check (§ 3).
- **Fixing anything.** The verb reports; it edits no workflow and starts no
  run.

## 10. What checks this

| Rule | What catches a breach |
|---|---|
| A flake is one workflow on one SHA | INV-1 |
| Budget threshold and job-span timing | INV-2 |
| Findings name runs and SHAs | INV-4 |
| Orient's block and its ETag | INV-7 |
| A capped fetch still reports | INV-8 |
| A failed fetch saves nothing | INV-9 |
| No workflows, no process | INV-10 |
| Served by `ants-mcpd`, no `MainWindow` | INV-11 |
| A shell guard is a budget; only at the start of `run:` | INV-12 |
| No push, re-run or lower-budget advice | INV-5, over the fixtures' messages only. **Partial:** a message from live data is not checked |
| Orient never contacts GitHub | INV-6 |
| Unknown budgets never guessed | INV-3 |
| One owner per check | **nothing** — a new check here duplicating `ci-gate` is caught only by review |
| `cache` hit or miss is right | **nothing** — depends on GitHub's log wording; `unknown` is the fallback |

## 11. Cross-doc impact

- `docs/standards/mcp-error-codes.md`: `github_unavailable`, and
  `runner_missing`'s row widened from `focused_test` to `ci_report`'s `gh`.
- `docs/standards/mcp-caches.md`: the saved-report row.
- `docs/standards/mcp-behavioural-notes.md`: a `ci_report` entry.
- `README.md`: the verb count (`tools/check-readme-claims.sh`).
- `CHANGELOG.md` `[Unreleased]`.
- `docs/subsystems.md`: the three new source pairs.
- After shipping, claude-config adds routing in `check-code` and
  `local-gate.md` § 9; it gets the verb name and § 4.7's refusals.

## 12. Reload

The verb holds no running state beyond the in-flight guard of § 4.5, so it
has no stop/start control. Its code runs in `ants-mcpd`, so a new build
reaches clients on their next reconnect after `ants-mcpd` is rebuilt, with
no terminal relaunch. Each call reads the workflow files and the saved
report afresh. The `session_orient` block is part of a verb `ants-mcpd`
already serves, and reaches clients the same way.

## 13. RAM / build cost

Nothing stays in memory between calls. During a call the fetched JSON is
bounded by the call cap (300) and the log cap (10 × 2 MiB). One saved file
per project, bounded by the caps on `runs`/`shas` and by the window. No new
library, no new build target beyond the feature test.

## 14. Cold-eyes loop log

Rows live in [`docs/reviews/ANTS-6399-ci-report-verb-loop-log.md`](../reviews/ANTS-6399-ci-report-verb-loop-log.md).
