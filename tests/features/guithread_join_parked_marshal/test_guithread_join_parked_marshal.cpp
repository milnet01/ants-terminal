// Feature-conformance test for spec.md — ANTS-5113.
//
// Behavioural, not a source scrape: the claim is about whether
// ants::joinRefusingMarshals actually returns while a worker is genuinely
// parked inside ants::onGuiThread's Qt::BlockingQueuedConnection wait, which
// only a real cross-thread call, timed against a real join, can show.
//
// Why this exists: ClaudeIntegration::shutdownDispatchWorker sets the
// refused flag, then joins the dispatch worker with a bare wait(). The flag
// only stops a marshal not yet posted; one already posted and parked is
// waiting for the GUI thread to service its queued call, and the GUI thread
// is waiting in the join — neither returns. This locks the fix:
// joinRefusingMarshals must return once the refused flag is set even when a
// worker is already parked in onGuiThread, that parked call must then
// resolve to std::nullopt, and the callable handed to it must never run.
// See spec.md "Timing" for the one soft spot: the test cannot prove the
// worker was parked before the flag was set without editing guithread.h, so
// it makes that ordering likely (busy-wait + a settle sleep) rather than
// certain.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

#include "guithread.h"

namespace {

// Resets the process-global refused-marshal flag on every exit path — it is
// shared across every TEST() in this bundle (src/guithread.h;
// tests/features/verify_trust_modal_gui_thread's GuiMarshalRefusedGuard does
// the same).
struct GuiMarshalRefusedResetGuard {
    ~GuiMarshalRefusedResetGuard() { ants::setGuiMarshalRefused(false); }
};

// Everything the worker thread's lambda and the callable it hands to
// onGuiThread() touch, heap-allocated so a worker that must be leaked
// (see the recovery path below) can never write into a test stack frame
// that has already unwound — the same reasoning as
// tests/features/verify_trust_modal_gui_thread's WorkerState.
struct SharedState {
    std::atomic<bool> callableRan{false};
    std::atomic<bool> workerAboutToCall{false};
    std::atomic<bool> workerFinished{false};
    std::atomic<bool> gotValue{false};
    std::atomic<bool> gotNullopt{false};
};

// How long the main thread waits, busy (no event pumping), for the worker to
// signal it has entered onGuiThread(). Setup-only bound, not an invariant.
constexpr int kWorkerStartBoundMs = 2000;

// Settle time between observing the worker has entered onGuiThread() and
// setting the refused flag — see spec.md "Timing" for why 150ms is expected
// to be enough margin over the microseconds onGuiThread needs to post.
constexpr int kParkSettleMs = 150;

// Bound passed to ants::joinRefusingMarshals for the call INV-1 measures.
constexpr int kJoinTimeoutMs = 500;

// Bound for the failure-path recovery (pump + real join) that keeps a
// failing run from leaving a parked thread behind.
constexpr int kRecoveryBoundMs = 5000;

}  // namespace

// INV-1/INV-2/INV-3 — joining a worker already parked inside onGuiThread,
// after the refused flag is set, must return promptly (INV-1); the parked
// call must then resolve to std::nullopt (INV-2); and the callable it was
// given must never run (INV-3).
TEST(GuithreadJoinParkedMarshal, JoinReturnsWithoutRunningParkedCallable) {
    ASSERT_NE(QCoreApplication::instance(), nullptr)
        << "no QCoreApplication instance — broken test bundle setup (see "
           "tests/bundle_main_core.cpp), not the defect this test targets.";

    GuiMarshalRefusedResetGuard resetGuard;
    ants::setGuiMarshalRefused(false);  // start clean regardless of run order

    auto state = std::make_unique<SharedState>();
    SharedState *const raw = state.get();

    auto callable = [raw]() -> int {
        raw->callableRan.store(true, std::memory_order_release);
        return 1;
    };

    // Raw QThread::create, matching this codebase's own convention
    // (src/luaengine.cpp, src/mainwindow.cpp, src/remotecontrol_state.cpp) —
    // held in a unique_ptr so every early return still owns cleanup, and
    // .release()'d instead of deleted on the one path where the worker could
    // still be running (Qt aborts on destructing a running QThread). Only
    // `raw` and `callable` (itself only capturing `raw`) cross into the
    // worker lambda — never a reference to a local on this stack frame.
    std::unique_ptr<QThread> worker(QThread::create([raw, callable]() {
        raw->workerAboutToCall.store(true, std::memory_order_release);
        std::optional<int> result = ants::onGuiThread(callable);
        if (result.has_value())
            raw->gotValue.store(true, std::memory_order_release);
        else
            raw->gotNullopt.store(true, std::memory_order_release);
        raw->workerFinished.store(true, std::memory_order_release);
    }));
    worker->start();

    // Wait for the worker to reach onGuiThread() — no event pumping here,
    // and none until after INV-1's join call returns. Pumping would service
    // the marshal directly and the parked scenario this test targets would
    // never happen (spec.md "Timing").
    {
        QElapsedTimer clock;
        clock.start();
        while (!raw->workerAboutToCall.load(std::memory_order_acquire)) {
            ASSERT_LT(clock.elapsed(), kWorkerStartBoundMs)
                << "worker thread never reached onGuiThread() within "
                << kWorkerStartBoundMs
                << "ms — broken test setup, not the defect this test "
                   "targets.";
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Settle so the worker has (very likely) cleared onGuiThread's own
    // guiMarshalRefused() check and posted its BlockingQueuedConnection call
    // before we flip the flag. See spec.md "Timing" — the one soft spot.
    std::this_thread::sleep_for(std::chrono::milliseconds(kParkSettleMs));

    ants::setGuiMarshalRefused(true);

    QElapsedTimer joinClock;
    joinClock.start();
    const bool joined =
        ants::joinRefusingMarshals(worker.get(), kJoinTimeoutMs);
    const qint64 joinElapsedMs = joinClock.elapsed();

    if (!joined) {
        ADD_FAILURE()
            << "INV-1: expected ants::joinRefusingMarshals(worker, "
            << kJoinTimeoutMs
            << ") to return once the refused flag is set, even though the "
               "worker is already parked inside onGuiThread's "
               "Qt::BlockingQueuedConnection wait. Expected: joined == true "
               "within " << kJoinTimeoutMs << "ms. Actual: it did not "
               "return within that bound (elapsed " << joinElapsedMs
            << "ms) — the worker is still parked waiting for the GUI "
               "thread to service its queued call, and the GUI thread is "
               "waiting in this join. This is the ANTS-5113 deadlock "
               "(joinRefusingMarshals is currently a bare worker->wait() "
               "stub in src/guithread.h with no way to notice or serve a "
               "parked marshal).";

        // Recover so a failing run does not hang the test binary or leak a
        // parked OS thread indefinitely: pump events to service the
        // marshal (this DOES run the callable against the current stub —
        // recovery is deliberately outside what INV-1/2/3 measure), then
        // join for real.
        {
            QElapsedTimer recoveryClock;
            recoveryClock.start();
            while (!raw->workerFinished.load(std::memory_order_acquire) &&
                   recoveryClock.elapsed() < kRecoveryBoundMs) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
        }
        if (worker->wait(kRecoveryBoundMs)) {
            return;  // real join succeeded — `state` destructs normally.
        }
        // Still stuck — leak both rather than destruct a running QThread
        // (Qt aborts on that) or free SharedState out from under it.
        // Mirrors verify_trust_modal_gui_thread's detached-and-leaked
        // worker on timeout.
        [[maybe_unused]] QThread *const leakedThread = worker.release();
        [[maybe_unused]] SharedState *const leakedState = state.release();
        return;
    }

    // Joined within the bound: the worker has fully exited (QThread::wait
    // returning true is a real join), so every atomic it wrote is safe to
    // read without further synchronisation.
    EXPECT_TRUE(raw->gotNullopt.load())
        << "INV-2: expected the parked onGuiThread() call to resolve to "
           "std::nullopt once joinRefusingMarshals unblocked it. Expected: "
           "nullopt. Actual: "
        << (raw->gotValue.load()
                ? "it returned a value instead of nullopt"
                : "neither nullopt nor a value was recorded — the worker "
                  "lambda did not run to completion");

    EXPECT_FALSE(raw->callableRan.load())
        << "INV-3: expected the callable handed to the parked onGuiThread() "
           "call to never run once the marshal was refused while parked. "
           "Expected: callableRan == false. Actual: callableRan == true — "
           "the callable executed despite the refused flag being set "
           "before the join completed.";
}
