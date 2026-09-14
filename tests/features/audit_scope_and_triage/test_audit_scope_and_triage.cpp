// Audit changed-lines scope and batch triage — see spec.md.
// ANTS-5084. Source-scrape of the AuditDialog sources.

#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {

QString auditSource() {
    return QString::fromStdString(ants_test::slurpAuditDialog());
}

// Body of the function whose definition starts with `signature`; the first
// line that is exactly "}" closes it.
QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    if (end < 0) return QString();
    return src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(AuditScopeAndTriage, RunKeepsSetsForSinceBaseline) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::runAudit()"));
    ASSERT_FALSE(body.isEmpty()) << "runAudit not found";
    EXPECT_TRUE(body.contains(QStringLiteral("m_recentOnly || m_sinceBaseline")))
        << "a run clears the changed-line sets the Since baseline pill reads";
}

// INV-2
TEST(AuditScopeAndTriage, GitFailureIsReported) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::computeRecentChangeSets("));
    ASSERT_FALSE(body.isEmpty()) << "computeRecentChangeSets not found";
    EXPECT_TRUE(body.contains(QStringLiteral("m_recentScopeError = runGit(")));
    EXPECT_TRUE(body.contains(QStringLiteral("QStringLiteral(\"hash-object\")")))
        << "a repository without HEAD~N gets no diff at all";
    EXPECT_TRUE(body.contains(QStringLiteral("p.kill()")))
        << "a timed-out git is left running";
}

// INV-3
TEST(AuditScopeAndTriage, FiltersStandDownOnError) {
    const QString src = auditSource();
    EXPECT_TRUE(src.contains(QStringLiteral(
        "if (m_recentOnly && m_recentScopeError.isEmpty() && !f.file.isEmpty())")));
    EXPECT_EQ(src.count(QStringLiteral("visibleSinceBaseline(f, m_recentLines")), 1)
        << "a Since baseline call site bypasses sinceBaselineVisible";
    const QString render = functionBody(src, QStringLiteral("void AuditDialog::renderResults()"));
    ASSERT_FALSE(render.isEmpty());
    EXPECT_TRUE(render.contains(QStringLiteral("Changed-lines filter off")));
}

// INV-4
TEST(AuditScopeAndTriage, BatchTriageIsBounded) {
    const QString src = auditSource();
    EXPECT_TRUE(src.contains(QStringLiteral("m_triageBatchQueue.append(")));
    const QString pump = functionBody(src, QStringLiteral("void AuditDialog::pumpTriageBatches()"));
    ASSERT_FALSE(pump.isEmpty()) << "pumpTriageBatches not found";
    EXPECT_TRUE(pump.contains(QStringLiteral("m_triageBatchesInFlight < kMaxTriageInFlight")));
    const QString batch = functionBody(src,
        QStringLiteral("void AuditDialog::requestAiTriageBatch("));
    ASSERT_FALSE(batch.isEmpty()) << "requestAiTriageBatch not found";
    EXPECT_FALSE(batch.contains(QStringLiteral("auto *mgr = new QNetworkAccessManager")))
        << "each batch builds its own network manager";
    EXPECT_TRUE(batch.contains(QStringLiteral("m_triageNam->post(")));
    EXPECT_FALSE(batch.contains(QStringLiteral("renderResults()")));
}

// INV-5
TEST(AuditScopeAndTriage, TriageRepliesAreCapped) {
    const QString src = auditSource();
    const QString single = functionBody(src,
        QStringLiteral("void AuditDialog::requestAiTriage(const QString"));
    const QString batch = functionBody(src,
        QStringLiteral("void AuditDialog::requestAiTriageBatch("));
    ASSERT_FALSE(single.isEmpty());
    ASSERT_FALSE(batch.isEmpty());
    EXPECT_TRUE(single.contains(QStringLiteral("capTriageReply(reply)")));
    EXPECT_TRUE(batch.contains(QStringLiteral("capTriageReply(reply)")));
    EXPECT_TRUE(src.contains(QStringLiteral("received > LlmClient::kMaxBytes")));
}
