// ANTS-1727 — LlmDispatcher scheduling feature test (injected fake runner,
// no network, no event loop).
//
// INV-6  never exceeds maxConcurrent in flight.
// INV-7  every job runs once; allFinished fires once.
// INV-8  dispatcher stores no result (source-grep).
// INV-9  cancelAll clears the queue + drains to allFinished.
// INV-14 maxConcurrent clamped to [1, 4].

#include "llmdispatcher.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QList>
#include <QString>

#include <algorithm>
#include <functional>
#include <vector>

namespace {

LlmJob job(const QString &id) {
    LlmJob j;
    j.id = id;
    return j;
}

QList<LlmJob> nJobs(int n) {
    QList<LlmJob> jobs;
    for (int i = 0; i < n; ++i) jobs << job(QString::number(i));
    return jobs;
}

}  // namespace

// INV-6 / INV-7 — peak concurrency ≤ max; exactly N jobFinished + 1
// allFinished.
TEST(LlmDispatcher, INV6_INV7_ConcurrencyAndCompleteness) {
    LlmDispatcher disp(2);
    int peak = 0;
    std::vector<std::function<void(const LlmResult &)>> pending;
    disp.setRunner([&](const LlmJob &, std::function<void(const LlmResult &)> done) {
        peak = std::max(peak, disp.inFlight());
        pending.push_back(std::move(done));
    });

    int jobFinished = 0;
    int allFinished = 0;
    QObject::connect(&disp, &LlmDispatcher::jobFinished,
                     [&](const QString &, const LlmResult &) { ++jobFinished; });
    QObject::connect(&disp, &LlmDispatcher::allFinished,
                     [&]() { ++allFinished; });

    disp.enqueue(nJobs(10));
    EXPECT_EQ(disp.inFlight(), 2);
    EXPECT_EQ(disp.pending(), 8);

    // Drain FIFO: firing each completion lets the next queued job start
    // (the runner appends its new completion to `pending`).
    std::size_t idx = 0;
    while (idx < pending.size()) {
        auto done = pending[idx++];
        LlmResult r;
        r.ok = true;
        done(r);
    }

    EXPECT_LE(peak, 2);
    EXPECT_EQ(peak, 2);
    EXPECT_EQ(jobFinished, 10);
    EXPECT_EQ(allFinished, 1);
    EXPECT_EQ(disp.inFlight(), 0);
    EXPECT_EQ(disp.pending(), 0);
}

// INV-6 — at maxConcurrent=1 the pool is strictly serial.
TEST(LlmDispatcher, INV6_SerialAtOne) {
    LlmDispatcher disp(1);
    int peak = 0;
    std::vector<std::function<void(const LlmResult &)>> pending;
    disp.setRunner([&](const LlmJob &, std::function<void(const LlmResult &)> done) {
        peak = std::max(peak, disp.inFlight());
        pending.push_back(std::move(done));
    });
    disp.enqueue(nJobs(5));
    std::size_t idx = 0;
    while (idx < pending.size()) {
        auto done = pending[idx++];
        LlmResult r;
        done(r);
    }
    EXPECT_EQ(peak, 1);
}

// INV-9 — cancelAll clears the queue, drains in-flight to allFinished, and
// emits no jobFinished for cancelled jobs.
TEST(LlmDispatcher, INV9_CancelAll) {
    LlmDispatcher disp(2);
    std::vector<std::function<void(const LlmResult &)>> pending;
    disp.setRunner([&](const LlmJob &, std::function<void(const LlmResult &)> done) {
        pending.push_back(std::move(done));
    });

    int jobFinished = 0;
    int allFinished = 0;
    QObject::connect(&disp, &LlmDispatcher::jobFinished,
                     [&](const QString &, const LlmResult &) { ++jobFinished; });
    QObject::connect(&disp, &LlmDispatcher::allFinished,
                     [&]() { ++allFinished; });

    disp.enqueue(nJobs(10));
    EXPECT_EQ(disp.inFlight(), 2);
    EXPECT_EQ(disp.pending(), 8);

    disp.cancelAll();
    EXPECT_EQ(disp.pending(), 0);   // queue cleared
    EXPECT_EQ(allFinished, 0);      // still 2 in flight

    // Fire the 2 in-flight completions; guarded → no jobFinished, drains.
    for (auto &done : pending) {
        LlmResult r;
        done(r);
    }
    EXPECT_EQ(jobFinished, 0);
    EXPECT_EQ(allFinished, 1);
    EXPECT_EQ(disp.inFlight(), 0);
}

// INV-14 (clamp) — maxConcurrent clamped to [1, 4].
TEST(LlmDispatcher, INV14_ClampMaxConcurrent) {
    EXPECT_EQ(LlmDispatcher(0).maxConcurrent(), 1);
    EXPECT_EQ(LlmDispatcher(-5).maxConcurrent(), 1);
    EXPECT_EQ(LlmDispatcher(2).maxConcurrent(), 2);
    EXPECT_EQ(LlmDispatcher(4).maxConcurrent(), 4);
    EXPECT_EQ(LlmDispatcher(99).maxConcurrent(), 4);
}

// INV-8 — dispatcher stores no LlmResult; forwards by const-ref.
TEST(LlmDispatcher, INV8_NoResultRetention) {
    const std::string h = ants_test::slurpFile(SRC_LLMDISPATCHER_H_PATH);
    ASSERT_FALSE(h.empty());
    // jobFinished forwards a const-ref result.
    EXPECT_NE(h.find("void jobFinished(const QString &id, const LlmResult &result)"),
              std::string::npos);
    // No container/member retaining results.
    EXPECT_EQ(h.find("QList<LlmResult>"), std::string::npos);
    EXPECT_EQ(h.find("QHash<QString, LlmResult>"), std::string::npos);
    EXPECT_EQ(h.find("QVector<LlmResult>"), std::string::npos);
    EXPECT_EQ(h.find("LlmResult m_"), std::string::npos);

    const std::string cpp = ants_test::slurpFile(SRC_LLMDISPATCHER_CPP_PATH);
    ASSERT_FALSE(cpp.empty());
    // Completion forwards `r` straight to the jobFinished signal.
    EXPECT_NE(cpp.find("emit jobFinished(id, r)"), std::string::npos);
}
