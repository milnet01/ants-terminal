// Audit suppressions take effect only when saved, read fresh lines, and are
// honoured everywhere a finding is shown or counted — see spec.md. ANTS-5083,
// ANTS-6381..6384. Mostly a source-scrape of the AuditDialog sources.

#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "auditengine.h"

namespace {

QString auditSource() {
    return QString::fromStdString(ants_test::slurpAuditDialog());
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

}  // namespace

// INV-1
TEST(AuditSuppressionSave, RunClearsTheLineCache) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::runAudit()"));
    ASSERT_FALSE(body.isEmpty()) << "runAudit not found";
    EXPECT_TRUE(body.contains(QStringLiteral("m_fileLineCache.clear()")))
        << "a run reuses the last run's cached lines, missing new inline suppressions";
}

// INV-2
TEST(AuditSuppressionSave, UnsavedSuppressionDoesNotHide) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("bool AuditDialog::saveSuppression("));
    ASSERT_FALSE(body.isEmpty()) << "saveSuppression not found";
    EXPECT_TRUE(body.contains(QStringLiteral("&& sf.commit()")))
        << "the rewrite ignores QSaveFile::commit()";
    const int guard = body.indexOf(QStringLiteral("if (!saved)"));
    const int insert = body.indexOf(QStringLiteral("m_suppressedKeys.insert(dedupKey)"));
    ASSERT_GE(guard, 0) << "saveSuppression never checks whether the save worked";
    ASSERT_GE(insert, 0);
    EXPECT_LT(guard, insert)
        << "the key is marked suppressed before checking the save";
}

// INV-3 — ANTS-6381
TEST(AuditSuppressionSave, ClaudeExportSkipsSuppressedFindings) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("QString AuditDialog::plainTextResults() const"));
    ASSERT_FALSE(body.isEmpty()) << "plainTextResults not found";
    EXPECT_TRUE(body.contains(QStringLiteral("isSuppressed(f)")))
        << "the Review-with-Claude export hands over suppressed findings";
}

// INV-4 — ANTS-6382
TEST(AuditSuppressionSave, LineAboveMarkersReachTheMatcher) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("bool AuditDialog::inlineSuppressed(const Finding &f) const"));
    ASSERT_FALSE(body.isEmpty()) << "inlineSuppressed not found";
    const int step3 = body.indexOf(QStringLiteral("const int prevIdx"));
    ASSERT_GE(step3, 0);
    const QString prev = body.mid(step3);
    EXPECT_TRUE(prev.contains(QStringLiteral("\"cppcheck-suppress\"")))
        << "a line-above cppcheck-suppress never reaches commentSuppresses";
    EXPECT_TRUE(prev.contains(QStringLiteral("\"nosemgrep\"")))
        << "a line-above nosemgrep never reaches commentSuppresses";
}

// INV-5 — ANTS-6383: suppressed findings take no slot under the cap.
TEST(AuditSuppressionSave, SuppressedFindingsDoNotCountAgainstTheCap) {
    CheckResult r;
    for (int i = 0; i < 9; ++i) {
        Finding f;
        f.line = i + 1;
        f.dedupKey = QString::number(i);
        f.suppressed = (i % 2 == 0);   // 5 suppressed, 4 shown
        r.findings.append(f);
    }
    AuditEngine::capFindings(r, 3);
    int shown = 0, hidden = 0;
    for (const Finding &f : r.findings) (f.suppressed ? hidden : shown)++;
    EXPECT_EQ(shown, 3);
    EXPECT_EQ(r.omittedCount, 1) << "suppressed findings were counted as omitted";
    EXPECT_EQ(hidden, 5) << "suppressed findings must stay for the SARIF export";
}

// INV-5 — ANTS-6383: the noise floor counts findings hidden this run.
TEST(AuditSuppressionSave, NoiseFloorCountsThisRunsSuppressions) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::renderResults()"));
    ASSERT_FALSE(body.isEmpty()) << "renderResults not found";
    EXPECT_FALSE(body.contains(QStringLiteral("totalSuppressed = m_suppressedKeys.size()")))
        << "the noise floor counts keys in .audit_suppress, not findings it hid";
}

// INV-6 — ANTS-6384
TEST(AuditSuppressionSave, FailedSaveStopsTheCaller) {
    const QString src = auditSource();
    EXPECT_TRUE(src.contains(QStringLiteral("bool AuditDialog::saveSuppression(")))
        << "saveSuppression cannot tell its caller the save failed";
    EXPECT_TRUE(src.contains(QStringLiteral("if (!saveSuppression(key, ruleId, reason)) return;")))
        << "the caller reports Suppressed after a failed save";
}

// INV-7 — ANTS-6385: a baseline in another format is not read as v2.
TEST(AuditSuppressionSave, OldBaselineIsNotReadAsCurrent) {
    const QString body = functionBody(auditSource(),
        QStringLiteral("void AuditDialog::loadBaseline()"));
    ASSERT_FALSE(body.isEmpty()) << "loadBaseline not found";
    const int version = body.indexOf(QStringLiteral("value(\"version\")"));
    const int insert = body.indexOf(QStringLiteral("m_baselineFingerprints.insert("));
    ASSERT_GE(version, 0) << "loadBaseline never checks the baseline's version";
    ASSERT_GE(insert, 0);
    EXPECT_LT(version, insert) << "the version is checked after the keys are loaded";
}
