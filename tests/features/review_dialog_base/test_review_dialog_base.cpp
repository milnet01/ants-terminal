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
    using ReviewDialogBase::setJobRunner;

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
