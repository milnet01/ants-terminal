// Feature-conformance test for tests/features/audit_inprocess_lanes_async/spec.md
// (ANTS-5067): the in-process drift lanes run off the calling thread, under a
// deadline, and a late result is discarded.

#include "auditrunner.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtGlobal>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifndef ANTS_AUDITDIALOG_SOURCES
#  error "ANTS_AUDITDIALOG_SOURCES compile definition required"
#endif
#ifndef SRC_AUDITRUNNER_CPP_PATH
#  error "SRC_AUDITRUNNER_CPP_PATH compile definition required"
#endif

using namespace std::chrono_literals;
using AuditRunner::internal::InProcessLane;
using AuditRunner::internal::InProcessLaneOutcome;
using AuditRunner::internal::runInProcessLanes;

namespace {

// A gate a fake lane blocks on until the test opens it.
struct Latch {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void release() {
        { std::lock_guard<std::mutex> l(m); open = true; }
        cv.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [this] { return open; });
    }
};

bool waitFor(const std::atomic<bool> &flag, int ms) {
    for (int waited = 0; waited < ms && !flag.load(); waited += 10)
        std::this_thread::sleep_for(10ms);
    return flag.load();
}

// The text from `from` up to the first `to` after it; empty if either is absent.
std::string between(const std::string &s, const std::string &from,
                    const std::string &to) {
    const std::size_t a = s.find(from);
    if (a == std::string::npos) return {};
    const std::size_t b = s.find(to, a + from.size());
    if (b == std::string::npos) return {};
    return s.substr(a, b - a);
}

// The in-process branch of AuditDialog::runNextCheck, comments stripped. It
// ends where the QProcess path starts resetting its accumulators.
std::string inProcessBranch(const std::string &dialog) {
    const std::string body =
        ants_test::slurpFunctionBody(dialog, "void AuditDialog::runNextCheck(");
    return between(body, "if (check.inProcessRunner)", "m_currentOutput.clear();");
}

QString laneRoot() { return QStringLiteral("/nonexistent-ants-5067"); }

}  // namespace

// INV-1 — the runner is called inside a QThread::create worker, and the old
// singleShot(0) deferral on the GUI thread is gone.
TEST(AuditInprocessLanesAsync, Inv1RunnerRunsOnAWorker) {
    const std::string branch =
        inProcessBranch(ants_test::stripComments(ants_test::slurpAuditDialog()));
    ASSERT_FALSE(branch.empty()) << "setup: in-process branch of runNextCheck not found";

    const std::size_t create = branch.find("QThread::create(");
    EXPECT_NE(create, std::string::npos)
        << "INV-1: the in-process branch does not start a QThread::create worker";
    EXPECT_EQ(branch.find("QTimer::singleShot"), std::string::npos)
        << "INV-1: the runner is still deferred onto the GUI thread";
    const std::size_t call = branch.find("runner(");
    ASSERT_NE(call, std::string::npos) << "setup: no runner call in the branch";
    EXPECT_TRUE(create != std::string::npos && call > create)
        << "INV-1: the runner is called outside the worker lambda";
}

// INV-2 — a late result is dropped by generation, not by m_cancelled, which
// runAudit resets.
TEST(AuditInprocessLanesAsync, Inv2LateResultDroppedByGeneration) {
    const std::string dialog = ants_test::stripComments(ants_test::slurpAuditDialog());
    const std::string branch = inProcessBranch(dialog);
    ASSERT_FALSE(branch.empty()) << "setup: in-process branch of runNextCheck not found";

    EXPECT_TRUE(branch.find("!= m_runGeneration") != std::string::npos ||
                branch.find("== m_runGeneration") != std::string::npos)
        << "INV-2: the delivery does not compare its generation with m_runGeneration";
    EXPECT_NE(ants_test::slurpFunctionBody(dialog, "void AuditDialog::runAudit(")
                  .find("++m_runGeneration"),
              std::string::npos)
        << "INV-2: AuditDialog::runAudit does not increment m_runGeneration";
    EXPECT_NE(ants_test::slurpFunctionBody(dialog, "void AuditDialog::cancelAudit(")
                  .find("++m_runGeneration"),
              std::string::npos)
        << "INV-2: AuditDialog::cancelAudit does not increment m_runGeneration";
}

// INV-3 — the call returns by its deadline while the lane is still blocked.
TEST(AuditInprocessLanesAsync, Inv3ReturnsByTheDeadlineWhileALaneRuns) {
    auto latch = std::make_shared<Latch>();
    auto returned = std::make_shared<std::atomic<bool>>(false);
    QList<InProcessLane> lanes;
    lanes.append({QStringLiteral("slow"), [latch, returned](const QString &) {
        latch->wait();
        returned->store(true);
        return QStringLiteral("late output");
    }});

    QElapsedTimer t;
    t.start();
    const QList<InProcessLaneOutcome> out = runInProcessLanes(lanes, laneRoot(), 200);
    const qint64 took = t.elapsed();
    const bool stillRunning = !returned->load();
    latch->release();

    ASSERT_EQ(out.size(), 1) << "INV-3: expected one outcome per lane";
    EXPECT_EQ(out[0].id, QStringLiteral("slow"));
    EXPECT_EQ(out[0].status, QStringLiteral("timed_out")) << "INV-3";
    EXPECT_TRUE(out[0].output.isEmpty()) << "INV-3: a timed-out lane carries no output";
    EXPECT_TRUE(stillRunning) << "INV-3: the call waited for the lane to finish";
    EXPECT_LT(took, 2200) << "INV-3: the call returned long after its 200 ms budget";
    EXPECT_TRUE(waitFor(*returned, 5000)) << "setup: the blocked lane never returned";
}

// INV-4 — lanes inside the budget are ok, keep their output, keep their order.
TEST(AuditInprocessLanesAsync, Inv4FastLanesAreOkInOrder) {
    QList<InProcessLane> lanes;
    lanes.append({QStringLiteral("a"), [](const QString &) { return QStringLiteral("A"); }});
    lanes.append({QStringLiteral("b"), [](const QString &) { return QStringLiteral("B"); }});

    const QList<InProcessLaneOutcome> out = runInProcessLanes(lanes, laneRoot(), 10000);
    ASSERT_EQ(out.size(), 2) << "INV-4: expected one outcome per lane";
    EXPECT_EQ(out[0].id, QStringLiteral("a"));
    EXPECT_EQ(out[1].id, QStringLiteral("b"));
    EXPECT_EQ(out[0].status, QStringLiteral("ok"));
    EXPECT_EQ(out[1].status, QStringLiteral("ok"));
    EXPECT_EQ(out[0].output, QStringLiteral("A"));
    EXPECT_EQ(out[1].output, QStringLiteral("B"));
}

// INV-5 — after the deadline, the abandoned worker does not start lane two.
TEST(AuditInprocessLanesAsync, Inv5AbandonedWorkerStartsNoFurtherLane) {
    auto latch = std::make_shared<Latch>();
    auto oneReturned = std::make_shared<std::atomic<bool>>(false);
    auto twoCalls = std::make_shared<std::atomic<int>>(0);
    QList<InProcessLane> lanes;
    lanes.append({QStringLiteral("one"), [latch, oneReturned](const QString &) {
        latch->wait();
        oneReturned->store(true);
        return QString();
    }});
    lanes.append({QStringLiteral("two"), [twoCalls](const QString &) {
        twoCalls->fetch_add(1);
        return QString();
    }});

    const QList<InProcessLaneOutcome> out = runInProcessLanes(lanes, laneRoot(), 150);
    latch->release();

    ASSERT_EQ(out.size(), 2) << "INV-5: expected one outcome per lane";
    EXPECT_EQ(out[0].status, QStringLiteral("timed_out"));
    EXPECT_EQ(out[1].status, QStringLiteral("timed_out"))
        << "INV-5: a lane after a timed-out lane must be timed_out";
    ASSERT_TRUE(waitFor(*oneReturned, 5000)) << "setup: lane one never returned";
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(twoCalls->load(), 0) << "INV-5: the abandoned worker ran lane two";
}

// INV-6 — no budget, no lane.
TEST(AuditInprocessLanesAsync, Inv6NoBudgetCallsNoLane) {
    for (const qint64 budget : {qint64(0), qint64(-5)}) {
        auto calls = std::make_shared<std::atomic<int>>(0);
        QList<InProcessLane> lanes;
        for (const char *id : {"x", "y"})
            lanes.append({QString::fromLatin1(id), [calls](const QString &) {
                calls->fetch_add(1);
                return QString();
            }});

        const QList<InProcessLaneOutcome> out = runInProcessLanes(lanes, laneRoot(), budget);
        std::this_thread::sleep_for(100ms);
        ASSERT_EQ(out.size(), 2) << "INV-6: expected one outcome per lane, budget " << budget;
        for (const InProcessLaneOutcome &o : out)
            EXPECT_EQ(o.status, QStringLiteral("timed_out")) << "INV-6, budget " << budget;
        EXPECT_EQ(calls->load(), 0) << "INV-6: a lane ran with budget " << budget;
    }
}

// INV-7 — a tool-less default sweep still gives the lanes a budget. Needs a
// process that has not resolved a tool before (resolveToolAbsolute caches).
TEST(AuditInprocessLanesAsync, Inv7ToolLessSweepGivesTheLanesABudget) {
    QTemporaryDir project;
    QTemporaryDir emptyBin;
    ASSERT_TRUE(project.isValid() && emptyBin.isValid());

    const QByteArray oldPath = qgetenv("PATH");
    qputenv("PATH", emptyBin.path().toLocal8Bit());
    AuditRunner::RunRequest req;
    req.projectRoot = project.path();
    const AuditRunner::RunResult r = AuditRunner::runAudit(req);
    qputenv("PATH", oldPath);

    ASSERT_TRUE(r.ok) << "setup: runAudit refused: " << r.code.toStdString()
                      << " " << r.error.toStdString();
    ASSERT_TRUE(r.byTool.contains(QStringLiteral("spec_code_drift")))
        << "setup: the spec_code_drift lane did not run";
    EXPECT_EQ(r.byTool.value(QStringLiteral("spec_code_drift")).status,
              QStringLiteral("ok"))
        << "INV-7: the lanes got no budget on a tool-less sweep";
}

// INV-8 — runAudit runs the lanes through runInProcessLanes and passes each
// outcome's status to finish, instead of a hard-coded "ok".
TEST(AuditInprocessLanesAsync, Inv8OutcomeStatusReachesFinish) {
    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_AUDITRUNNER_CPP_PATH, "RunResult runAudit(const RunRequest &req) {"));
    ASSERT_FALSE(body.empty()) << "setup: runAudit body not found";

    const std::string lanes = between(body, "runInProcessLanes(", "r.totalRaw");
    ASSERT_FALSE(lanes.empty()) << "INV-8: runAudit does not call runInProcessLanes";
    EXPECT_NE(lanes.find("finish("), std::string::npos)
        << "INV-8: lane outcomes do not go through finish";
    EXPECT_NE(lanes.find(".status"), std::string::npos)
        << "INV-8: finish is not given the outcome's status";
    EXPECT_EQ(lanes.find("QStringLiteral(\"ok\")"), std::string::npos)
        << "INV-8: a lane status is still hard-coded to ok";
}
