# ANTS-5067 — Run the in-process drift lanes off the calling thread, under a deadline

**Status:** spec draft (2026-09-14).
**Kind:** review-fix.
**Source:** ROADMAP.md ANTS-5067 (code-quality-review-2026-09-11 perf pass, lanes spec-engines and audit-dialog-b; user decisions 2026-09-14).
**Composes with:** ANTS-1351 (`audit_run` threading model and aggregate cap).

## 1. Problem

1. `AuditDialog::runNextCheck` calls `AuditCheck::inProcessRunner` on the
   GUI thread, inside a `QTimer::singleShot(0, …)` lambda. While a lane
   runs, no tab drains terminal output and Cancel cannot be clicked.
2. `AuditRunner::runAudit` runs the `kInProcessLanes` table after
   `aggTimer` has stopped, calling each lane synchronously. Nothing bounds
   them, so a slow lane runs the audit past its aggregate cap.
3. The aggregate cap in `runAudit` counts only external tools
   (`toolAbsPath`). A default sweep with no external tool installed
   computes a cap of zero.

`tests/perf/bench_drift_lanes.cpp` measures the lanes' cost; the roadmap item
records the latest run. The user decided on 2026-09-14 to move the lanes
off the GUI thread and under the cap whatever that cost is.

## 2. Surface

### 2.1 Decision: abandon, not stop

On cancel or timeout, the caller stops waiting and discards the lane's
result. The lane runs to its natural end on its own thread.

Rejected: stopping the lane through a cancel flag passed into
`FeatureCoverage`.

- Stopping changes every `FeatureCoverage::run*Check` signature, and the
  helpers they call (`buildProjectSourceBlob`, `buildSourceIndex`). Each
  needs cancel checks placed where they cannot corrupt a result.
- A lane reads files and holds only function-local `static const` data
  in `src/featurecoverage.cpp`. A lane left running touches nothing its
  caller owns.
- The wasted work is bounded by one lane's run time.

Cost accepted: an abandoned lane keeps a core busy until it finishes. An
audit started meanwhile runs alongside it.

### 2.2 Audit dialog — `src/auditdialog.cpp`, `src/auditdialog.h`

- New member `quint64 m_runGeneration`. `AuditDialog::runAudit` and
  `AuditDialog::cancelAudit` each increment it.
- The in-process branch of `runNextCheck` captures the current
  generation. It runs the runner on a `QThread::create` worker and
  delivers the output through a queued connection on `QThread::finished`,
  with `this` as context. The pattern is `AuditDialog::requestDebtScan`.
- The worker lambda captures copies only: the project path, the runner,
  and a `std::shared_ptr<QString>` result slot.
- The delivery calls `handleCheckOutput` only when its captured
  generation equals `m_runGeneration`. Otherwise it returns.
- The worker deletes itself on `finished`. `MainWindow` opens the dialog
  with `Qt::WA_DeleteOnClose`, so the dialog can be destroyed mid-lane;
  the `this` context then drops the delivery.
- The `QTimer::singleShot(0, …)` deferral goes. It existed so the status
  label painted before a blocking call, and nothing blocks now.

### 2.3 `audit_run` — `src/auditrunner.cpp`, `src/auditrunner.h`

New seam in `AuditRunner::internal`, so the deadline is testable without
a slow real lane:

```cpp
struct InProcessLane {
    QString id;
    std::function<QString(const QString & /*projectRoot*/)> fn;
};

struct InProcessLaneOutcome {
    QString id;
    QString status;        // "ok" or "timed_out"
    QString output;        // empty when "timed_out"
    qint64  elapsedMs = 0;
};

// Runs `lanes` in order on one worker thread. Returns one outcome per
// lane, in input order, no later than `budgetMs` after the call.
QList<InProcessLaneOutcome> runInProcessLanes(const QList<InProcessLane> &lanes,
                                              const QString &projectRoot,
                                              qint64 budgetMs);
```

- A lane unfinished at the deadline, and every lane after it, is
  `timed_out`. The worker is abandoned.
- An abandoned worker starts no further lane. It checks a shared flag
  between lanes, never inside one.
- The worker owns its shared state, not the caller's stack.
  `LuaEngine::runQueryThreaded` is the precedent for detaching a late
  worker. The suite's leak gate stays green.
- `budgetMs <= 0` starts no worker. Every lane is `timed_out`.

`runAudit` changes:

- When the in-process lanes run (empty `req.tools`, `scopeNarrowed`
  false), the aggregate cap counts them as tools:
  `aggCapMs = min((toolAbsPath.size() + laneCount) * perToolMs * 3 / 2, kAggregateCapMs)`.
- A `QElapsedTimer` starts beside `aggTimer`. The lanes' budget is
  `aggCapMs` minus that timer's elapsed time.
- Each outcome goes through the existing `finish(id, status, output,
  elapsedMs)`. A `timed_out` lane lands in `incomplete_tools` and sets
  `partial`, like a timed-out external tool.

## 3. Invariants

- **INV-1** — The dialog never calls an `inProcessRunner` on the GUI
  thread. Broken by any call to the runner in `runNextCheck` outside the
  worker lambda. *Test:* `tests/features/audit_inprocess_lanes_async`,
  source scrape of `AuditDialog::runNextCheck`.
- **INV-2** — A lane result from an earlier run is discarded. Cancel,
  then Run, while a lane is still working: its late output is not added
  to the new run. Broken by a delivery that checks `m_cancelled` alone,
  which `runAudit` resets. *Test:*
  `tests/features/audit_inprocess_lanes_async`, source scrape: the
  delivery compares its captured generation with `m_runGeneration`, and
  both `runAudit` and `cancelAudit` increment it.
- **INV-3** — `runInProcessLanes` returns by its deadline while a lane is
  still running, and reports that lane `timed_out` with empty output.
  Broken by a helper that joins the worker. *Test:*
  `tests/features/audit_inprocess_lanes_async`: a fake lane blocks on a
  latch the test holds; the call returns with the latch still held.
- **INV-4** — Lanes finishing inside the budget are `ok`, carry their
  output, and come back in input order. Broken by dropped output or
  reordering. *Test:* `tests/features/audit_inprocess_lanes_async`, two
  fast fake lanes with distinct output.
- **INV-5** — An abandoned worker starts no further lane. Broken by a
  worker that runs the remaining lanes after the deadline. *Test:*
  `tests/features/audit_inprocess_lanes_async`: lane one blocks on a
  latch, lane two counts its calls. After the call returns, the test
  releases the latch, waits for lane one to report it returned, and
  checks lane two's count stays zero over a short window.
- **INV-6** — `budgetMs <= 0` calls no lane. Broken by a helper that
  starts the worker first. *Test:*
  `tests/features/audit_inprocess_lanes_async`: every fake lane counts
  calls; the count is zero and every outcome is `timed_out`.
- **INV-7** — A default sweep with no external tool gives the in-process
  lanes a non-zero budget. Broken by leaving the lanes out of the cap
  formula, which reports every lane `timed_out`. *Test:*
  `tests/features/audit_inprocess_lanes_async`: `runAudit` on a temp
  project with `PATH` pointed at an empty directory reports
  `spec_code_drift` with status `ok`. It runs in a process that has not
  resolved a tool before, because `resolveToolAbsolute` caches results
  per process in `g_toolResolveCache`.
- **INV-8** — `runAudit` routes every lane outcome's status through
  `finish`, so a timed-out lane marks the run partial. Broken by
  hard-coding `ok`, as the current loop does. *Test:*
  `tests/features/audit_inprocess_lanes_async`, source scrape of
  `AuditRunner::runAudit`.

## 4. RAM / build cost

No new build target: the test joins the `test_claude` bundle beside the
other `audit_run` tests. No new Qt component: `QThread::create` is QtCore.

An abandoned lane holds its source blob and index until it finishes. A
Run started right after a cancel can therefore hold a second copy for up
to one lane's run.

## 5. Out of scope

- A deadline on in-process lanes inside the dialog — excluded. Cancel is
  the escape there, and it now works while a lane runs.
- Stopping a lane mid-run — rejected in § 2.1.
- One shared blob and index across the two contract-doc lanes —
  excluded here. The ANTS-5067 item records why a path-keyed cache is
  unsafe; the idea needs its own roadmap item.
- Running the lanes concurrently with the external tools — excluded, so
  the tool event loop in `runAudit` stays as it is.

## 6. Tests

Feature test: `tests/features/audit_inprocess_lanes_async/`, in the
`test_claude` bundle. Covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6,
INV-7 and INV-8. Verify each test fails against
pre-fix source first.

INV-7 is the exception: today's lanes run unbounded, so its test passes
before the change. It fails only under the mutation named in its clause,
and that mutation run is its red proof.

## 7. Cross-doc impact

- `src/auditrunner.h` header comment, "Threading model" — `runAudit` now
  starts a worker for the in-process lanes.
- `docs/specs/ANTS-1351.md` — check its threading invariant against
  § 2.3.
- `CHANGELOG.md` — a `### Fixed` entry.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
