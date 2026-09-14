# Feature spec: ANTS-5077 — a stuck parse thread is detached on tab close, never terminated

`TerminalWidget::~TerminalWidget` quits the tab's parse thread and waits up to
2 s. When the worker was still running after that, it called
`QThread::terminate()` and then waited without a bound. `terminate()` can kill
the worker while it holds a lock (the allocator's, or one of Qt's), and the GUI
then deadlocks on that lock.

**Decided (2026-09-14, user):** a parse thread still running after the 2 s wait
is detached and cleans itself up.

This touches the teardown path ANTS-1189 hardened
(`tests/features/pty_dtor_off_main_thread/spec.md`). That spec explains why a
worker left running during app exit can race Qt teardown. The detach runs only
when the bounded wait has already failed, which that spec's own reap budget
keeps rare.

## Invariants under test

- **INV-1 — the destructor never terminates the thread.** The body of
  `TerminalWidget::~TerminalWidget` contains no `terminate(` call.
- **INV-2 — a thread still running after the wait is detached.** The branch
  taken when `m_parseThread->wait(2000)` fails unparents the thread with
  `setParent(nullptr)`, so the widget's destruction does not delete a running
  `QThread` (which aborts), and connects `QThread::finished` to
  `QObject::deleteLater`.
- **INV-3 — the GUI does not block after the wait.** That branch contains no
  unbounded `wait()`.
- **INV-4 — a thread that finishes before the connection is still deleted.**
  That branch checks `isFinished()`, so a worker that ended between the failed
  wait and the connection is not leaked.

## Test surface

Source scrape of `TerminalWidget::~TerminalWidget` in `src/terminalwidget.cpp`,
comments stripped. A behavioural test needs a GUI widget whose worker outlives
a 2 s wait, which the unit bundles cannot build reliably.

## Red proof

Before the fix every invariant fails: the branch calls `terminate()` and an
unbounded `wait()`, and neither unparents nor deletes the thread.
