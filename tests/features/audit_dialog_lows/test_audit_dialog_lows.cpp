// Audit dialog detection walk, labels and exports — see spec.md.
// ANTS-5084. Source-scrape of the AuditDialog sources.

#include <QRegularExpression>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {

QString auditSource() {
    return QString::fromStdString(ants_test::slurpAuditDialog());
}

QString between(const QString &src, const QString &from, const QString &to) {
    const int a = src.indexOf(from);
    if (a < 0) return QString();
    const int b = src.indexOf(to, a);
    if (b < 0) return QString();
    return src.mid(a, b - a);
}

}  // namespace

// INV-1
TEST(AuditDialogLows, DetectionWalkIsLevelBounded) {
    const QString region = between(auditSource(),
        QStringLiteral("auto hasAnyFile = [&]"),
        QStringLiteral("hasAnyFile({\"Dockerfile\""));
    ASSERT_FALSE(region.isEmpty()) << "hasAnyFile not found";
    EXPECT_FALSE(region.contains(QStringLiteral("QDirIterator")))
        << "detection walks the whole tree before applying its depth cap";
    EXPECT_TRUE(region.contains(QStringLiteral("QDir::NoSymLinks")));
}

// INV-2
TEST(AuditDialogLows, BannerHasNoDoubledPercent) {
    const QString src = auditSource();
    ASSERT_FALSE(src.isEmpty());
    EXPECT_FALSE(src.contains(QStringLiteral("%% actionable")));
}

// INV-3
TEST(AuditDialogLows, HtmlPayloadEscapesEveryLessThan) {
    const QString region = between(auditSource(),
        QStringLiteral("QString AuditDialog::exportHtml()"),
        QStringLiteral("kTemplate"));
    ASSERT_FALSE(region.isEmpty()) << "exportHtml not found";
    EXPECT_TRUE(region.contains(QStringLiteral("\"\\\\u003c\"")))
        << "only </ is escaped; <!-- can still blank the report";
}

// INV-4
TEST(AuditDialogLows, WarningIsNotCalledATimeout) {
    const QString src = auditSource();
    EXPECT_FALSE(src.contains(QStringLiteral("\" (timeout)\"")));
    EXPECT_TRUE(src.contains(QStringLiteral("\" (tool issue)\"")));
}

// INV-5
TEST(AuditDialogLows, FailedExportOpenIsReported) {
    const QString src = auditSource();
    EXPECT_EQ(src.count(QStringLiteral("\"SARIF save failed: \"")), 2);
    EXPECT_EQ(src.count(QStringLiteral("\"HTML save failed: \"")), 2);
}

// INV-6
TEST(AuditDialogLows, RecentFilesMatchAtSeparator) {
    const QString region = between(auditSource(),
        QStringLiteral("void AuditDialog::handleCheckOutput("),
        QStringLiteral("drop findings whose line isn't within a diff hunk"));
    ASSERT_FALSE(region.isEmpty()) << "handleCheckOutput recent filter not found";
    EXPECT_TRUE(region.contains(QStringLiteral("pathSuffixMatches(f.file, rf)")));
    EXPECT_FALSE(region.contains(QStringLiteral("f.file.endsWith(rf)")));
}

// INV-7
TEST(AuditDialogLows, NoStackProcessOutlivesItsTimeout) {
    const QString src = auditSource();
    ASSERT_FALSE(src.isEmpty());
    static const QRegularExpression stackProcess(QStringLiteral(R"(\n\s+QProcess \w+;)"));
    const QRegularExpressionMatch m = stackProcess.match(src);
    EXPECT_FALSE(m.hasMatch())
        << "a stack QProcess waits for its child again on destruction: "
        << m.captured(0).trimmed().toStdString();
    // The definition, the stat probe's call and the git runner's deleter.
    EXPECT_GE(src.count(QStringLiteral("releaseProcess")), 3)
        << "the stat probe and the git runner no longer release their process";
}

// INV-8
TEST(AuditDialogLows, SemgrepSendsNoMetrics) {
    const QString region = between(auditSource(),
        QStringLiteral("\"semgrep\", \"Semgrep (structural patterns)\""),
        QStringLiteral("CheckType::Vulnerability"));
    ASSERT_FALSE(region.isEmpty()) << "semgrep catalogue entry not found";
    EXPECT_TRUE(region.contains(QStringLiteral("--metrics=off")))
        << "registry rule packs send usage metrics under semgrep's default";
}

// INV-10
TEST(AuditDialogLows, FilterTypingIsDebounced) {
    const QString region = between(auditSource(),
        QStringLiteral("connect(m_filterInput, &QLineEdit::textChanged"),
        QStringLiteral("filterRow->addWidget(m_filterInput"));
    ASSERT_FALSE(region.isEmpty()) << "filter input handler not found";
    EXPECT_FALSE(region.contains(QStringLiteral("renderResults()")))
        << "every keystroke re-renders the whole result list";
    EXPECT_TRUE(region.contains(QStringLiteral("m_filterDebounce->start()")));
}

// INV-9
TEST(AuditDialogLows, SuppressionSaveIsLocked) {
    const QString region = between(auditSource(),
        QStringLiteral("void AuditDialog::saveSuppression("),
        QStringLiteral("QByteArray existingBytes;"));
    ASSERT_FALSE(region.isEmpty()) << "saveSuppression not found";
    EXPECT_TRUE(region.contains(QStringLiteral("ConfigWriteLock lock(path);")))
        << "two instances saving suppressions can interleave the read and the rewrite";
}
