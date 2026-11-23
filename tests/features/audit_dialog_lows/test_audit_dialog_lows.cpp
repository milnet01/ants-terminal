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
    const QString body = between(src,
        QStringLiteral("void AuditDialog::saveReportPrivately("),
        QStringLiteral("\n}\n"));
    ASSERT_FALSE(body.isEmpty()) << "saveReportPrivately not found";
    EXPECT_EQ(body.count(QStringLiteral("label + \" save failed: \" + sf.errorString()")), 2)
        << "a failed open or a failed commit is not reported";
    EXPECT_TRUE(src.contains(QStringLiteral("saveReportPrivately(QStringLiteral(\"SARIF\")")));
    EXPECT_TRUE(src.contains(QStringLiteral("saveReportPrivately(QStringLiteral(\"HTML\")")));
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
        QStringLiteral("bool AuditDialog::saveSuppression("),
        QStringLiteral("QByteArray existingBytes;"));
    ASSERT_FALSE(region.isEmpty()) << "saveSuppression not found";
    EXPECT_TRUE(region.contains(QStringLiteral("ConfigWriteLock lock(path);")))
        << "two instances saving suppressions can interleave the read and the rewrite";
}

// ANTS-6386 — settings are read through liveConfig(), never a fresh local
// Config, which re-reads the file and misses in-memory changes (ANTS-2003).
TEST(AuditDialogLows, SettingsComeFromTheLiveConfig) {
    const QString src = auditSource();
    static const QRegularExpression freshLocal(QStringLiteral(R"(\bConfig\s+\w+\s*;)"));
    const QRegularExpressionMatch m = freshLocal.match(src);
    EXPECT_FALSE(m.hasMatch()) << "a fresh Config local: " << m.captured().toStdString();
    EXPECT_TRUE(src.contains(QStringLiteral("Config &AuditDialog::liveConfig()")));
}

// INV-12 — ANTS-6195
TEST(AuditDialogLows, TrendSnapshotIsTakenOncePerRun) {
    const QString src = QString::fromStdString(
        ants_test::stripComments(ants_test::slurpAuditDialog()));
    const QString render = between(src,
        QStringLiteral("void AuditDialog::renderResults() {"), QStringLiteral("\n}\n"));
    ASSERT_FALSE(render.isEmpty()) << "renderResults not found";
    EXPECT_FALSE(render.contains(QStringLiteral("loadLastSnapshot(")))
        << "a re-render after the run's own save compares the run with itself";
    EXPECT_FALSE(render.contains(QStringLiteral("appendSnapshot(")))
        << "the render writes trend.json";
    const QString finish = between(src,
        QStringLiteral("void AuditDialog::finishRun() {"), QStringLiteral("\n}\n"));
    ASSERT_FALSE(finish.isEmpty()) << "finishRun not found";
    const qsizetype load = finish.indexOf(QStringLiteral("loadLastSnapshot("));
    const qsizetype save = finish.indexOf(QStringLiteral("appendSnapshot("));
    const qsizetype draw = finish.indexOf(QStringLiteral("renderResults()"));
    ASSERT_GE(load, 0) << "finishRun does not read the previous snapshot";
    ASSERT_GE(save, 0) << "finishRun does not save this run's snapshot";
    EXPECT_LT(load, save) << "the previous snapshot is read after this run's is saved";
    EXPECT_LT(load, draw) << "the run's first render has no previous snapshot to compare with";
}

// INV-13 — ANTS-6196
TEST(AuditDialogLows, CommentFilterIsSetAtRegistration) {
    const QString src = QString::fromStdString(
        ants_test::stripComments(ants_test::slurpAuditDialog()));
    EXPECT_FALSE(src.contains(QStringLiteral("kSourceScannedChecks")))
        << "an id list, kept apart from the checks, decides the comment filter";
    EXPECT_TRUE(src.contains(QStringLiteral(
        "if (check.codeLinesOnly) dropFindingsInCommentsOrStrings(r);")));
    const QString grep = between(src,
        QStringLiteral("void AuditDialog::addGrepCheck("), QStringLiteral("\n}\n"));
    ASSERT_FALSE(grep.isEmpty()) << "addGrepCheck not found";
    EXPECT_TRUE(grep.contains(QStringLiteral("c.codeLinesOnly = true;")))
        << "a grep check is not comment-filtered by default";
    const QString marker = between(src,
        QStringLiteral("addGrepCheck(\"conflict_markers\""), QStringLiteral("m_checks.append"));
    ASSERT_FALSE(marker.isEmpty()) << "conflict_markers not found";
    EXPECT_TRUE(marker.contains(QStringLiteral("m_checks.last().codeLinesOnly = false;")))
        << "a conflict marker inside a block comment is dropped";
    for (const char *id : {"debug_leftovers", "cmd_injection_dyn", "unsafe_deser"}) {
        const QString reg = between(src,
            QStringLiteral("\"%1\", \"").arg(QLatin1String(id)), QStringLiteral("m_checks.append"));
        EXPECT_TRUE(reg.contains(QStringLiteral("m_checks.last().codeLinesOnly = true;")))
            << id << " lost its comment filter";
    }
    const QString rules = between(src,
        QStringLiteral("int AuditDialog::loadUserRules()"), QStringLiteral("\n}\n"));
    ASSERT_FALSE(rules.isEmpty()) << "loadUserRules not found";
    EXPECT_TRUE(rules.contains(QStringLiteral("c.codeLinesOnly = m_checks[i].codeLinesOnly;")))
        << "a user rule replacing a built-in check drops its comment filter";
}
