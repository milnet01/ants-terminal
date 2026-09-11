// Feature-conformance test for spec.md — ANTS-5025.
//
// Behavioural, not a source scrape: the claim is about which thread actually
// runs showPrompt(), which only a real cross-thread call can show. A
// RecordingModalClient subclass overrides showPrompt() to record its calling
// thread and return a canned Decision, so nothing here ever constructs a
// QMessageBox or blocks on user input.
//
// Why this exists: ModalClient::prompt() calls showPrompt() directly on the
// caller's thread. verify_changes runs off the GUI thread (ANTS-2132), so an
// untrusted .ants/verify.json builds a QMessageBox on the MCP worker —
// undefined behaviour in Qt. This locks the fix: the dialog body must run on
// the GUI thread, and a refused GUI marshal must short-circuit to Headless
// without ever calling showPrompt().

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "guithread.h"
#include "verifytrustmodal.h"

namespace {

// A ModalClient that never shows a dialog: showPrompt() records which thread
// ran it and how many times, then returns a canned outcome. prompt() and
// showPrompt() themselves are untouched — this only observes the call.
class RecordingModalClient : public VerifyTrust::ModalClient {
public:
    explicit RecordingModalClient(const QString &trustFilePath)
        : VerifyTrust::ModalClient(/*parent*/ nullptr, trustFilePath) {}

    std::atomic<QThread *> showPromptThread{nullptr};
    std::atomic<int>       showPromptCalls{0};
    VerifyTrust::Outcome   returnOutcome = VerifyTrust::Outcome::UntrustedFellBack;

protected:
    VerifyTrust::Decision showPrompt(const QString &projectPath,
                                     const QString &shaHex,
                                     const QByteArray &configBytes) override {
        Q_UNUSED(projectPath);
        Q_UNUSED(configBytes);
        showPromptThread.store(QThread::currentThread(),
                               std::memory_order_release);
        showPromptCalls.fetch_add(1, std::memory_order_acq_rel);
        return {returnOutcome, shaHex};
    }
};

// Resets the process-global refused-marshal flag on every exit path
// (src/guithread.h: "the refused flag is process-global; a test that sets it
// must reset it on every exit path").
struct GuiMarshalRefusedGuard {
    explicit GuiMarshalRefusedGuard(bool value) { ants::setGuiMarshalRefused(value); }
    ~GuiMarshalRefusedGuard() { ants::setGuiMarshalRefused(false); }
};

// Everything a worker thread's call into outcomeForConfig() touches, heap-
// allocated so an abandoned (timed-out, detached) worker can never run past
// the end of a stack frame.
struct WorkerState {
    explicit WorkerState(const QString &trustFilePath)
        : client(trustFilePath) {}

    RecordingModalClient                client;
    QString                             projectPath;
    QByteArray                          cfgBytes;
    std::promise<VerifyTrust::Decision> prom;
};

struct WaitResult {
    bool completed = false;
    // Non-null only when completed==true — ownership handed back so the
    // caller can inspect `client` and let it destruct normally at end of
    // scope. Null on timeout: the worker was detached, not joined, and the
    // WorkerState it may still be touching is deliberately leaked rather
    // than freed out from under it (a heap-use-after-free would turn an
    // already-diagnosed test failure into a confusing crash under ASan).
    std::unique_ptr<WorkerState> state;
    VerifyTrust::Decision decision;  // only meaningful when completed==true
};

// Spawns a worker thread that calls state->client.outcomeForConfig() and
// pumps the GUI/main thread's event loop until it returns or `boundMs`
// elapses. The test_chrome bundle's QApplication never runs exec()
// (tests/bundle_main_gui.cpp), so a cross-thread BlockingQueuedConnection
// reply is only ever delivered by a caller pumping AllEvents — the same
// technique tests/features/mcp_async_dispatch/'s callVerb() uses.
//
// Never blocks past `boundMs`: a hang in the code under test must fail the
// test, not join the test process.
WaitResult waitOrAbandon(std::unique_ptr<WorkerState> state, int boundMs) {
    std::future<VerifyTrust::Decision> fut = state->prom.get_future();
    WorkerState *const raw = state.get();
    std::thread worker([raw]() {
        raw->prom.set_value(
            raw->client.outcomeForConfig(raw->projectPath, raw->cfgBytes));
    });

    QElapsedTimer clock;
    clock.start();
    while (fut.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        if (clock.elapsed() >= boundMs) {
            worker.detach();
            // Leaked deliberately — see WaitResult comment.
            [[maybe_unused]] WorkerState *const leaked = state.release();
            return {false, nullptr, {}};
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    worker.join();
    VerifyTrust::Decision decision = fut.get();
    return {true, std::move(state), decision};
}

constexpr int kBoundMs = 5000;

}  // namespace

// INV-1 — the dialog body (showPrompt) runs on the GUI thread when
// outcomeForConfig() is called from a worker thread.
TEST(VerifyTrustModalGuiThread, Inv1DialogBodyRunsOnGuiThread) {
    QTemporaryDir trustHome;
    ASSERT_TRUE(trustHome.isValid());
    const QString trustPath =
        trustHome.path() + QStringLiteral("/verify-trust.json");

    auto state = std::make_unique<WorkerState>(trustPath);
    state->client.returnOutcome = VerifyTrust::Outcome::UntrustedFellBack;
    state->projectPath = QStringLiteral("/some/proj-inv1");
    state->cfgBytes = "{\"build\":{\"command\":\"echo inv1-gui-thread\"}}";

    QThread *const guiThread = QThread::currentThread();

    WaitResult result = waitOrAbandon(std::move(state), kBoundMs);
    if (!result.completed) {
        ADD_FAILURE()
            << "INV-1: outcomeForConfig() called from a worker thread did "
               "not return within " << kBoundMs << "ms. Expected: a prompt "
               "on an unrefused GUI marshal completes quickly. Actual: the "
               "call never returned — a GUI-marshal deadlock, not the fast "
               "defect this test targets.";
        return;
    }

    const RecordingModalClient &client = result.state->client;

    EXPECT_EQ(client.showPromptCalls.load(), 1)
        << "INV-1: expected showPrompt() to run exactly once; it ran "
        << client.showPromptCalls.load() << " time(s).";

    QThread *const ranOn =
        client.showPromptThread.load(std::memory_order_acquire);
    EXPECT_EQ(ranOn, guiThread)
        << "INV-1: expected the dialog body (showPrompt) to run on the GUI "
           "thread (" << static_cast<const void *>(guiThread) << "), but it "
           "ran on " << static_cast<const void *>(ranOn) << " — the calling "
           "(worker) thread. ModalClient::prompt() must marshal "
           "showPrompt() through ants::onGuiThread instead of calling it "
           "directly on the caller's thread.";
}

// INV-2 — a refused GUI marshal yields Outcome::Headless, and showPrompt()
// is never invoked.
TEST(VerifyTrustModalGuiThread, Inv2RefusedMarshalYieldsHeadlessWithoutRunningBody) {
    QTemporaryDir trustHome;
    ASSERT_TRUE(trustHome.isValid());
    const QString trustPath =
        trustHome.path() + QStringLiteral("/verify-trust.json");

    auto state = std::make_unique<WorkerState>(trustPath);
    state->client.returnOutcome = VerifyTrust::Outcome::UntrustedFellBack;
    state->projectPath = QStringLiteral("/some/proj-inv2");
    state->cfgBytes = "{\"build\":{\"command\":\"echo inv2-refused-marshal\"}}";

    GuiMarshalRefusedGuard refusedGuard(/*value*/ true);

    WaitResult result = waitOrAbandon(std::move(state), kBoundMs);
    if (!result.completed) {
        ADD_FAILURE()
            << "INV-2: outcomeForConfig() called from a worker thread with "
               "the GUI marshal refused did not return within " << kBoundMs
            << "ms. Expected: a refused marshal short-circuits to Headless "
               "immediately. Actual: the call never returned.";
        return;
    }

    const RecordingModalClient &client = result.state->client;

    EXPECT_EQ(result.decision.outcome, VerifyTrust::Outcome::Headless)
        << "INV-2: expected Outcome::Headless (value "
        << static_cast<int>(VerifyTrust::Outcome::Headless)
        << ") when the GUI marshal is refused, got outcome="
        << static_cast<int>(result.decision.outcome) << ".";

    EXPECT_EQ(client.showPromptCalls.load(), 0)
        << "INV-2: expected showPrompt() to never run when the GUI marshal "
           "is refused; it ran " << client.showPromptCalls.load()
        << " time(s). A refused marshal must be checked before the dialog "
           "body runs, not after.";
}
