# ANTS-2131 — Every blocking MCP verb dispatches off the main thread

## Background

ANTS-2103/2104 moved the two MCP verbs that spin a **local `QEventLoop`**
(`audit_run`, `indie_review_dispatch`) onto `QThread::create` workers, so
their nested loops never pump the main thread and never reentrantly free
the live MCP `QLocalSocket` mid-dispatch (the use-after-free crash class
locked by ANTS-2102).

ANTS-2131 is the structural end-state. A second group of MCP verbs does
**not** spin a `QEventLoop` but still runs synchronously on the
dispatching (main) thread and **blocks on `QProcess::waitForFinished`**:

- `verify_changes` — shells out to per-gate build/test commands
  (`verifyengine.cpp` `waitForFinished`).
- `debt_sweep_scan` — shells out to `git` (`debtsweepengine.cpp:66`).
- `debt_sweep_apply_fix` — shells out to a packaging script
  (`debtsweepengine.cpp:945`).
- `debt_sweep_defer`, `debt_sweep_triage_prompt` — fast and in-process,
  wrapped too for family uniformity + future-safety.

`waitForFinished` blocks on the child-process pipe and does **not** pump
the main loop's socket notifiers, so — unlike the `QEventLoop` verbs —
these cannot trigger the reentrant-disconnect use-after-free. The cost is
a frozen GUI for the duration of the child process, not a crash. Moving
them to a joined worker thread (`QThread::wait()` is a join, not an event
pump) keeps the GUI responsive and closes the "any MCP verb that blocks
the main thread" class by construction.

The verbs the original roadmap bullet *guessed* were affected
(`cold_eyes_*`, `test_audit_*`) pump nothing at all — pure in-process —
so they are intentionally **out of scope** (wrapping them buys nothing).

## Mechanism

`mainwindow.cpp` registers the `rcDelegate`-shaped verbs through one of
two factories:

- `rcDelegate(fn)` — forwards `args` to `(m_remoteControl->*fn)(args)`
  synchronously on the dispatching thread.
- `rcDelegateWorker(fn)` (ANTS-2131) — same shim, but runs the `cmd*()`
  call inside a `QThread::create` lambda and `worker->wait()`-joins
  before returning the serialised result. The `QJsonDocument` result is a
  main-thread local captured by reference; the `QProcess` constructs and
  lives on the worker.

The five blocking verbs above register through `rcDelegateWorker`; every
other RC-shim verb stays on `rcDelegate`.

## Contract

- **INV-1** — `mainwindow.cpp` defines an `rcDelegateWorker` factory whose
  body runs the delegated `cmd*()` call inside a `QThread::create` worker
  and joins it with `worker->wait()` (a join, never an event pump).
- **INV-2** — `verify_changes` is registered through
  `rcDelegateWorker(&RemoteControl::cmdVerifyChanges)`, not the
  synchronous `rcDelegate`.
- **INV-3** — all four `debt_sweep_*` verbs (`scan`, `apply_fix`,
  `defer`, `triage_prompt`) are registered through `rcDelegateWorker`.

Reverting any of these to the synchronous `rcDelegate` re-freezes the GUI
and fails this test.

## Out of scope

- The `QEventLoop` worker-isolation for `audit_run` /
  `indie_review_dispatch` — that is ANTS-2103/2104, locked by ANTS-2102
  (INV-3/INV-4 there).
- `cold_eyes_*` / `test_audit_*` — pump nothing; not wrapped.
- Full async dispatch (no join at all) — a larger follow-up; the join
  keeps the existing one-request-at-a-time semantics.

## Test

`test_mcp_verb_offthread_guard.cpp` — pure source-grep against
`mainwindow.cpp` (path supplied as `SRC_MAINWINDOW_CPP_PATH` by the
`test_claude` bundle). No build-time runtime dependency, matching the
ANTS-2102 precedent.
