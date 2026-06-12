// ANTS-1727 — ReviewDialogBase feature test (GUI bundle).
//
// INV-12 endpointDispatchable predicate.
// INV-13 allocateFoldInIds returns [] + reason on counter failure, no write.
// INV-15 dispatchOne fires its callback without onAllReportsCollected.

#include "reviewdialogbase.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

namespace {

// Minimal concrete subclass exposing the protected services the test
// drives. derivePartition/composeBrief are trivial; onAllReportsCollected
// records its call count so INV-15 can assert it never fires.
class TestReviewDialog : public ReviewDialogBase {
public:
    using ReviewDialogBase::ReviewDialogBase;  // inherit (cwd, parent, config)

    using ReviewDialogBase::allocateFoldInIds;
    using ReviewDialogBase::dispatchOne;
    using ReviewDialogBase::lastFoldInError;
    using ReviewDialogBase::redispatch;
    using ReviewDialogBase::reports;
    using ReviewDialogBase::setJobRunner;
    using ReviewDialogBase::setLanes;

    int allCollectedCalls = 0;

protected:
    QList<ReviewLane> derivePartition() override { return {}; }
    LlmRequest composeBrief(const ReviewLane &) override { return {}; }
    void onAllReportsCollected(const QHash<QString, QString> &) override {
        ++allCollectedCalls;
    }
    void performFoldIn() override {}
};

QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

bool writeFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(content.toUtf8());
    return true;
}

}  // namespace

// INV-12 — dispatchable iff non-empty AND http/https.
TEST(ReviewDialogBase, INV12_EndpointDispatchable) {
    EXPECT_TRUE(ReviewDialogBase::endpointDispatchable(
        QStringLiteral("https://api.openai.com/v1/chat/completions")));
    EXPECT_TRUE(ReviewDialogBase::endpointDispatchable(
        QStringLiteral("http://localhost:11434/v1")));
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QString()));
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QStringLiteral("file:///x")));
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QStringLiteral("ftp://h/x")));
}

// INV-13 — corrupt counter → empty IDs, reason surfaced, file untouched.
TEST(ReviewDialogBase, INV13_AllocateFoldInIdsCounterFailure) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString counter = tmp.path() + QStringLiteral("/.roadmap-counter");
    ASSERT_TRUE(writeFile(counter, QStringLiteral("not-a-number\n")));

    TestReviewDialog dlg(tmp.path(), nullptr, nullptr);
    const QList<int> ids = dlg.allocateFoldInIds(2);
    EXPECT_TRUE(ids.isEmpty());
    EXPECT_FALSE(dlg.lastFoldInError().isEmpty()) << "reason not surfaced";
    // No write: corrupt counter is left intact.
    EXPECT_EQ(readFile(counter), QStringLiteral("not-a-number\n"));
}

// INV-15 — dispatchOne invokes its callback once and never enters
// onAllReportsCollected.
TEST(ReviewDialogBase, INV15_DispatchOneBypassesBatchComplete) {
    TestReviewDialog dlg(QString(), nullptr, nullptr);
    dlg.setJobRunner([](const LlmJob &,
                        std::function<void(const LlmResult &)> done) {
        LlmResult r;
        r.ok = true;
        r.text = QStringLiteral("synth-result");
        done(r);
    });

    int cbCalls = 0;
    LlmJob job;
    job.id = QStringLiteral("synthesis");
    dlg.dispatchOne(job, [&](const LlmResult &r) {
        ++cbCalls;
        EXPECT_EQ(r.text, QStringLiteral("synth-result"));
    });

    EXPECT_EQ(cbCalls, 1);
    EXPECT_EQ(dlg.allCollectedCalls, 0)
        << "dispatchOne must not re-enter onAllReportsCollected";
}

// INV-16 (ANTS-1843) — setLanes preserves already-collected reports for
// lanes that survive a re-partition / lane toggle, and only drops reports
// whose lane is gone.
TEST(ReviewDialogBase, INV16_SetLanesPreservesSurvivingReports) {
    TestReviewDialog dlg(QString(), nullptr, nullptr);
    // Synchronous runner so redispatch fills reports() before it returns.
    dlg.setJobRunner([](const LlmJob &job,
                        std::function<void(const LlmResult &)> done) {
        LlmResult r; r.ok = true;
        r.text = QStringLiteral("R:") + job.id;
        done(r);
    });

    dlg.setLanes({ {"A", "A", ""}, {"B", "B", ""}, {"C", "C", ""} });
    dlg.redispatch({ "A", "B", "C" });
    ASSERT_EQ(dlg.reports().size(), 3) << "all three lanes collected";

    // Toggle: keep A and C, drop B.
    dlg.setLanes({ {"A", "A", ""}, {"C", "C", ""} });
    EXPECT_EQ(dlg.reports().size(), 2);
    EXPECT_TRUE(dlg.reports().contains(QStringLiteral("A")));
    EXPECT_TRUE(dlg.reports().contains(QStringLiteral("C")));
    EXPECT_FALSE(dlg.reports().contains(QStringLiteral("B")))
        << "removed lane's report must be dropped";
    EXPECT_EQ(dlg.reports().value(QStringLiteral("A")), QStringLiteral("R:A"))
        << "surviving lane keeps its collected text";
}

// INV-17 (ANTS-1843) — startDispatch refreshes the partition once up-front
// (so per-lane briefFor can't re-partition mid-loop) via the prepareDispatch
// hook. Source-scrape: wiring is GUI-bound and not worth a live dispatch.
TEST(ReviewDialogBase, INV17_StartDispatchCallsPrepareDispatch) {
    QFile f(QStringLiteral(SRC_REVIEWDIALOGBASE_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString src = QString::fromUtf8(f.readAll());
    const int s = src.indexOf(QStringLiteral("void ReviewDialogBase::startDispatch("));
    ASSERT_GE(s, 0);
    const int e = src.indexOf(QStringLiteral("m_dispatcher->enqueue"), s);
    ASSERT_GT(e, s);
    EXPECT_TRUE(src.mid(s, e - s).contains(QStringLiteral("prepareDispatch()")))
        << "startDispatch must call prepareDispatch() before enqueuing jobs";
}

// INV-18 (ANTS-1843) — the Dispatch button re-evaluates on window
// re-activation, so fixing ai_endpoint in config.json re-enables it without
// a reopen. Source-scrape (window activation is unreliable offscreen).
TEST(ReviewDialogBase, INV18_ChangeEventRechecksDispatch) {
    QFile f(QStringLiteral(SRC_REVIEWDIALOGBASE_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString src = QString::fromUtf8(f.readAll());
    const int s = src.indexOf(QStringLiteral("void ReviewDialogBase::changeEvent("));
    ASSERT_GE(s, 0) << "changeEvent override missing";
    const int e = src.indexOf(QStringLiteral("\n}"), s);  // end of function
    ASSERT_GT(e, s);
    const QString body = src.mid(s, e - s);
    EXPECT_TRUE(body.contains(QStringLiteral("ActivationChange")));
    EXPECT_TRUE(body.contains(QStringLiteral("updateDispatchEnabled()")));
}

// INV-19 (ANTS-2111) — the runner tracks its spawned LlmClients and the
// destructor aborts them BEFORE m_dispatcher->cancelAll(), closing the
// close-mid-review UAF. Source-scrape: the race needs a live QNetworkReply
// in flight, which is not reproducible offscreen without real network I/O.
TEST(ReviewDialogBase, INV19_DtorAbortsOwnClientsBeforeCancelAll) {
    QFile f(QStringLiteral(SRC_REVIEWDIALOGBASE_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString src = QString::fromUtf8(f.readAll());

    // The runner must register each new client for teardown.
    const int rs = src.indexOf(QStringLiteral("m_runner = ["));
    ASSERT_GE(rs, 0) << "runner lambda missing";
    const int re = src.indexOf(QStringLiteral("m_dispatcher->setRunner"), rs);
    ASSERT_GT(re, rs);
    EXPECT_TRUE(src.mid(rs, re - rs)
                    .contains(QStringLiteral("m_activeClients.append(client)")))
        << "runner must track spawned clients for dtor abort";

    // The destructor must abort tracked clients before cancelAll. Anchor on
    // the actual call statements (c->abort() / m_dispatcher->cancelAll()) so
    // explanatory comments mentioning abort/cancelAll don't skew the order.
    const int ds = src.indexOf(QStringLiteral("ReviewDialogBase::~ReviewDialogBase("));
    ASSERT_GE(ds, 0) << "destructor missing";
    const int abortPos = src.indexOf(QStringLiteral("c->abort();"), ds);
    const int cancelPos = src.indexOf(QStringLiteral("m_dispatcher->cancelAll();"), ds);
    ASSERT_GE(abortPos, 0) << "destructor must abort its own clients";
    ASSERT_GE(cancelPos, 0) << "destructor must still cancelAll the dispatcher";
    EXPECT_LT(abortPos, cancelPos)
        << "own-client abort must precede dispatcher cancelAll";
}
