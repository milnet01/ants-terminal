// ANTS-6911 — Help → Report an Issue. See spec.md.

#include "reportissue.h"

#include "../../_support/srcgrep.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <gtest/gtest.h>

namespace {

ReportIssue::Environment sampleEnv() {
    ReportIssue::Environment env;
    env.appVersion = QStringLiteral("9.9.9 (commit abc1234)");
    env.qtVersion = QStringLiteral("6.11.0");
    env.system = QStringLiteral("Test Linux 1+1, kernel 7.0");
    env.desktop = QStringLiteral("KDE (wayland)");
    return env;
}

QString queryValue(const QUrl &url, const char *key) {
    return QUrlQuery(url).queryItemValue(QLatin1String(key), QUrl::FullyDecoded);
}

QString readTemplate(const char *name) {
    QFile f(QStringLiteral(ANTS_ISSUE_TEMPLATE_DIR "/") + QLatin1String(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

TEST(ReportIssue, HelpMenuOpensTheDialogWithTheLogPath) {
    const std::string help = ants_test::slurpFunctionBody(
        SRC_MAINWINDOW_MENUS_PATH, "void MainWindow::setupHelpMenu()");
    ASSERT_FALSE(help.empty()) << "could not locate setupHelpMenu()'s body";
    EXPECT_NE(help.find("\"Report an &Issue...\""), std::string::npos) << "INV-1";
    EXPECT_NE(help.find("ReportIssue::show(this, DebugLog::logFilePath())"),
              std::string::npos) << "INV-1";
}

TEST(ReportIssue, LinkNamesTheRightForm) {
    const QUrl bug = ReportIssue::issueUrl(ReportIssue::Kind::Bug, QStringLiteral("x"), sampleEnv());
    EXPECT_EQ(bug.host(), QStringLiteral("github.com"));
    EXPECT_EQ(bug.path(), QStringLiteral("/milnet01/ants-terminal/issues/new"));
    EXPECT_EQ(queryValue(bug, "template"), QStringLiteral("bug_report.yml")) << "INV-2";
    const QUrl idea = ReportIssue::issueUrl(ReportIssue::Kind::Idea, QStringLiteral("x"), sampleEnv());
    EXPECT_EQ(queryValue(idea, "template"), QStringLiteral("feature_request.yml")) << "INV-2";
}

TEST(ReportIssue, TitleAndDetailsSurviveEncoding) {
    const QString title = QStringLiteral("Tabs & splits: 50% + more #2 = ünïcode?");
    const ReportIssue::Environment env = sampleEnv();
    const QUrl url = ReportIssue::issueUrl(ReportIssue::Kind::Bug, title, env);
    EXPECT_EQ(queryValue(url, "title"), title) << "INV-3";
    EXPECT_EQ(queryValue(url, "environment"), env.toText()) << "INV-3";
    // A bare '+' would reach GitHub as a space; '#' would end the query.
    const QByteArray wire = url.toEncoded();
    EXPECT_FALSE(wire.contains('+')) << "INV-3: " << wire.constData();
    EXPECT_FALSE(wire.contains('#')) << "INV-3: " << wire.constData();
    EXPECT_TRUE(url.fragment().isEmpty()) << "INV-3";
}

TEST(ReportIssue, BlankTitleIsLeftOut) {
    const QUrl url = ReportIssue::issueUrl(ReportIssue::Kind::Bug, QStringLiteral("  "), sampleEnv());
    EXPECT_FALSE(QUrlQuery(url).hasQueryItem(QStringLiteral("title")));
}

TEST(ReportIssue, EachLinkedFormHasTheEnvironmentField) {
    for (const char *name : {"bug_report.yml", "feature_request.yml"}) {
        const QString yml = readTemplate(name);
        ASSERT_FALSE(yml.isEmpty()) << "INV-4: cannot read " << name;
        EXPECT_TRUE(yml.contains(QStringLiteral("id: environment\n"))) << "INV-4: " << name;
    }
}

TEST(ReportIssue, LogWarningOnlyWhenALogExists) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("debug.log"));

    QDialog *none = ReportIssue::createDialog(nullptr, logPath);
    EXPECT_EQ(none->findChild<QLabel *>(QStringLiteral("reportIssueLogWarning")), nullptr)
        << "INV-5: warning shown with no log";
    EXPECT_EQ(none->findChild<QPushButton *>(QStringLiteral("reportIssueOpenLogFolder")), nullptr);
    delete none;

    // The rotated copy alone counts as a log.
    QFile rotated(logPath + QStringLiteral(".1"));
    ASSERT_TRUE(rotated.open(QIODevice::WriteOnly));
    rotated.write("secret-folder-name\n");
    rotated.close();

    QDialog *withLog = ReportIssue::createDialog(nullptr, logPath);
    const auto *warning = withLog->findChild<QLabel *>(QStringLiteral("reportIssueLogWarning"));
    ASSERT_NE(warning, nullptr) << "INV-5: no warning with a log present";
    EXPECT_TRUE(warning->text().contains(QStringLiteral("private"))) << "INV-5";
    EXPECT_NE(withLog->findChild<QPushButton *>(QStringLiteral("reportIssueOpenLogFolder")), nullptr)
        << "INV-5";
    delete withLog;

    // The link never carries the log's path or its contents.
    const QUrl url = ReportIssue::issueUrl(ReportIssue::Kind::Bug, QStringLiteral("t"),
                                           ReportIssue::currentEnvironment());
    const QString decoded = url.toString(QUrl::FullyDecoded);
    EXPECT_FALSE(decoded.contains(dir.path())) << "INV-5";
    EXPECT_FALSE(decoded.contains(QStringLiteral("secret-folder-name"))) << "INV-5";
}
