# Feature: joining a worker after refusing marshals must not deadlock on a marshal already parked

## Problem

`ClaudeIntegration::shutdownDispatchWorker` (`src/claudeintegration.cpp`)
tears down the dispatch worker in what its own comment calls "the one order
that cannot deadlock": set `ants::setGuiMarshalRefused(true)`, then
`ants::joinRefusingMarshals(m_dispatchWorker)` from the GUI thread. The
refused flag only stops a marshal *not yet posted* — `ants::onGuiThread`
(`src/guithread.h`) checks it once, on the calling thread, before posting the
`Qt::BlockingQueuedConnection` call. A worker that has already passed that
check and posted is parked waiting for the GUI thread to service the queued
call. The GUI thread, meanwhile, is parked in the join. Neither side can move.

`ants::joinRefusingMarshals` is currently a stub
(`src/guithread.h`, `joinRefusingMarshals`) that does nothing but
`worker->wait(...)` — it has no way to notice or serve a parked marshal, so it
times out (bounded call) or hangs forever (the production call site,
`shutdownDispatchWorker`, passes no timeout). Filed as ANTS-5113, split out of
ANTS-5024 by two lanes of the 2026-09-11 performance pass. **Not reproduced
live** — reproducing it means hanging or force-quitting a live instance on
exit; this test reaches the same two functions directly instead, without
`ClaudeIntegration` or a `MainWindow`.

## The fix this test locks

The ROADMAP entry's proposed fix (unverified at authoring time): the queued
marshal's callable checks the refused flag again, on the GUI thread, right
before calling the caller's callable — skipping it once the flag is set —
and `joinRefusingMarshals` services posted meta-calls for the application
object (i.e. pumps the queue) while it waits for the worker to exit, instead
of blocking blind. Whatever the exact mechanism, three things must become
true together once the refused flag is set and `joinRefusingMarshals` is
called against a worker already parked inside `ants::onGuiThread`:

1. `joinRefusingMarshals` returns, instead of blocking on a wait that nothing
   will ever satisfy.
2. The parked `onGuiThread` call — on the worker's side — resolves to
   `std::nullopt`, exactly like a marshal refused before it was ever posted.
3. The callable handed to `onGuiThread` never runs.

This test does not touch `guithread.h` or `claudeintegration.cpp`; it drives
`ants::onGuiThread` and `ants::joinRefusingMarshals` directly, the same two
free functions `shutdownDispatchWorker` calls.

## Invariants

- **INV-1 — joining a parked marshal returns.** Once
  `ants::setGuiMarshalRefused(true)` has been called, a call to
  `ants::joinRefusingMarshals(worker, timeoutMs)` on the GUI thread returns
  within `timeoutMs`, even when `worker` is already parked inside
  `ants::onGuiThread`'s `Qt::BlockingQueuedConnection` wait (posted before the
  flag was set). *Test:* `guithread_join_parked_marshal` (paired test file).
- **INV-2 — the parked call resolves to `std::nullopt`.** When INV-1's join
  returns, the worker's `ants::onGuiThread` call — the one that was parked —
  has itself returned, and its result is `std::nullopt`, not the callable's
  value. *Test:* `guithread_join_parked_marshal` (paired test file).
- **INV-3 — the parked callable never runs.** The callable handed to the
  parked `ants::onGuiThread` call in INV-1/INV-2 is never invoked. *Test:*
  `guithread_join_parked_marshal` (paired test file).

## Scope

In scope: the interaction between `ants::onGuiThread`, `ants::setGuiMarshalRefused`
/ `ants::guiMarshalRefused`, and `ants::joinRefusingMarshals` when a marshal is
posted *before* the refused flag is set and the join happens *after*. This is
exactly `shutdownDispatchWorker`'s ordering and exactly the case its own
comment claims cannot deadlock.

Out of scope:
- `ants::onGuiThread`'s ordinary contract (direct call when already on the
  GUI thread, `std::nullopt` on no `QCoreApplication`, refusing a marshal
  that has not yet posted) — unchanged by this defect and not retested here.
- `ClaudeIntegration::shutdownDispatchWorker` itself, `ClaudeIntegration`
  construction/destruction, and `MainWindow` — the test calls the two free
  functions in `src/guithread.h` directly.
- A live hang/force-quit repro on a real running instance — the ROADMAP entry
  says this was not reproduced live; this test reaches the same code path
  synchronously from a plain worker thread instead.
- What the eventual fix's mechanism actually is. The three invariants above
  are stated as outcomes, not as an implementation (no assertion here reaches
  into `joinRefusingMarshals`'s internals or asserts against a `processEvents`
  call count).

### Timing — the one soft spot

INV-1/2/3 only test something when the worker is genuinely parked inside
`onGuiThread` *before* the refused flag is set — the note atop
`onGuiThread` in `guithread.h` said as much before the fix: "the flag above
refuses only marshals not yet posted; one already posted is ANTS-5113." If the flag
is set first, `onGuiThread` refuses at its own top-of-function check before
ever posting, the worker returns immediately, and every one of INV-1/2/3
holds trivially — the test would pass whether or not `joinRefusingMarshals`
does anything about a parked marshal at all.

There is no public hook for "the queued call has been posted and the worker
is now blocked waiting for it" — `QCoreApplication`'s event queue exposes no
portable pre-post signal, and adding one would mean editing `guithread.h`,
which this test does not do. The test instead makes the ordering **likely**
rather than certain: the worker thread sets an atomic
(`workerAboutToCall`) immediately on entering its lambda, before calling
`ants::onGuiThread`; the main/GUI thread busy-waits (no event pumping — see
below) on that atomic, then sleeps a further 150ms before setting the refused
flag. Everything `onGuiThread` does between the atomic store and posting the
`Qt::BlockingQueuedConnection` call — a thread-identity comparison and one
atomic load — is microseconds of work, so 150ms of margin is expected to be
enough on any machine this runs on. It is not a proof: a sufficiently
starved worker thread could still be scheduled after the 150ms window
closes, in which case the test would pass on both the current stub and any
future fix, offering neither signal. This is a known, accepted gap — not
something the assertions paper over — and is why the failure path
(`ADD_FAILURE` on `INV-1`) names the exact expected/actual, so a spurious
failure reads as diagnosable rather than as a flake.

The main/GUI thread must not call `QCoreApplication::processEvents()` at any
point between starting the worker and finishing the `joinRefusingMarshals`
call whose return is being timed for INV-1 — pumping would serve the parked
marshal itself and the scenario never happens. Recovery after a *failing*
`joinRefusingMarshals` call (see Test) does pump events, deliberately, to
release the worker and avoid leaving a parked thread behind; that pump
happens only after INV-1 has already been recorded as failed, so it plays no
part in the assertions above.

## Test

`test_guithread_join_parked_marshal.cpp`, in the `test_core` bundle
(`Qt6::Core`-only — `guithread.h` needs no widget code, so this test needs
no `ants_chrome_lib`). One `TEST()` drives the whole scenario:

1. Start a `QThread` whose run function stores `workerAboutToCall = true`,
   then calls `ants::onGuiThread(callable)` where `callable` records that it
   ran and returns a value, then stores whether the result was a value or
   `std::nullopt`, then stores `workerFinished = true`.
2. The main/GUI thread busy-waits on `workerAboutToCall` (bounded, with a
   diagnosable `ASSERT_LT` on timeout — this bound is setup-only and is not
   itself part of any invariant), then sleeps 150ms without pumping events
   (see Timing above), then calls `ants::setGuiMarshalRefused(true)`.
3. The main/GUI thread calls `ants::joinRefusingMarshals(worker, 500)` and
   times it.
   - If it returns `false` (does not join within 500ms): `ADD_FAILURE()`
     naming INV-1, the 500ms bound, and the measured elapsed time — this is
     the expected outcome against the current stub. The test then recovers
     rather than leaving a parked thread behind: it pumps
     `QCoreApplication::processEvents()` (bounded) to serve the marshal —
     which, against the stub, *does* run the callable, since recovery is
     explicitly not part of what INV-1/2/3 measure — joins the `QThread` for
     real, and returns. If recovery itself cannot get the worker to finish
     within its own bound, the `QThread` is leaked (never `delete`d) rather
     than destructed while still running, mirroring
     `tests/features/verify_trust_modal_gui_thread/`'s detached-and-leaked
     worker on timeout — Qt aborts on destructing a running `QThread`, and a
     leaked pointer is a smaller cost than a crashed test binary.
   - If it returns `true`: INV-1 holds. The worker has fully exited (`wait`
     is a real join), so its recorded result and `callableRan` flag are read
     without further synchronisation, and INV-2 / INV-3 are checked with
     `EXPECT_*`, each naming expected vs. actual on failure.

`ants::setGuiMarshalRefused(false)` is reset on every exit path via an RAII
guard (mirrors `verify_trust_modal_gui_thread`'s `GuiMarshalRefusedGuard` —
the flag is process-global, and this bundle runs many `TEST()`s in one
binary).

## Regression history

- **ANTS-2132 (`shutdownDispatchWorker` introduced):** the comment atop it
  claims "the one order that cannot deadlock" — refuse first, then join.
  True only for a marshal not yet posted when the flag is set.
- **ANTS-5024 (2026-09-11):** per-call hang from calling `onGuiThread` off a
  thread the GUI thread is blocked joining, found by the same review pass.
- **ANTS-5113 (2026-09-11, still open at authoring time):** this defect,
  split out of ANTS-5024 because it is specifically about `joinRefusingMarshals`
  and teardown rather than about a single call. `joinRefusingMarshals` is
  currently a stub (`worker->wait(...)` and nothing else) — this test is
  written to fail against that stub and pass once it can unblock a worker
  already parked in `onGuiThread`.
