// Audit dialog detection walk, labels and exports — see spec.md.
// ANTS-5084. Source-scrape of the AuditDialog sources.

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
