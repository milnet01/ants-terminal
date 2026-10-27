// A split pane's shell is tracked and released like a tab's — see spec.md.
// ANTS-5079. Source-scrape of the MainWindow sources.

#include <QFile>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"


namespace {

QString source() {
    return QString::fromStdString(ants_test::slurpMainWindow());
}

#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif
#ifndef SRC_TERMINALWIDGET_H_PATH
#define SRC_TERMINALWIDGET_H_PATH ""
#endif

QString readFile(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Brace-balanced body from the first `{` after the signature.
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
TEST(SplitPaneShellTracking, SplitPaneIsTracked) {
    const QString body = functionBody(
        source(), QStringLiteral("void MainWindow::splitCurrentPane("));
    ASSERT_FALSE(body.isEmpty()) << "splitCurrentPane not found";
    EXPECT_TRUE(body.contains(QStringLiteral("trackTerminalShell(newTerm)")));
}

// INV-2
TEST(SplitPaneShellTracking, ClosingAPaneReleasesIt) {
    const QString body = functionBody(
        source(), QStringLiteral("void MainWindow::closeFocusedPane("));
    ASSERT_FALSE(body.isEmpty()) << "closeFocusedPane not found";
    const int release = body.indexOf(QStringLiteral("releaseTerminalShell(focused)"));
    const int del = body.indexOf(QStringLiteral("focused->deleteLater()"));
    ASSERT_GE(release, 0) << "closeFocusedPane releases nothing";
    ASSERT_GE(del, 0);
    EXPECT_LT(release, del);
}

// INV-3
TEST(SplitPaneShellTracking, ClosingATabReleasesEveryPane) {
    const QString body = functionBody(
        source(), QStringLiteral("void MainWindow::performTabClose("));
    ASSERT_FALSE(body.isEmpty()) << "performTabClose not found";
    EXPECT_TRUE(body.contains(QStringLiteral("findChildren<TerminalWidget *>(")));
    EXPECT_TRUE(body.contains(QStringLiteral("releaseTerminalShell(")));
    EXPECT_FALSE(body.contains(QStringLiteral("untrackShell(term->shellPid())")))
        << "tab close still releases only the active pane";
}

// INV-4
TEST(SplitPaneShellTracking, ReleaseClearsAllThreeTrackers) {
    const QString body = functionBody(
        source(), QStringLiteral("void MainWindow::releaseTerminalShell("));
    ASSERT_FALSE(body.isEmpty()) << "releaseTerminalShell not found";
    EXPECT_TRUE(body.contains(QStringLiteral("untrackShell(")));
    EXPECT_TRUE(body.contains(QStringLiteral("untrackBgShell(")));
    EXPECT_TRUE(body.contains(QStringLiteral("forgetShell(")));
}

// INV-5
TEST(SplitPaneShellTracking, ReleaseFallsBackToTheRecordedPid) {
    const QString body = functionBody(
        source(), QStringLiteral("void MainWindow::releaseTerminalShell("));
    ASSERT_FALSE(body.isEmpty()) << "releaseTerminalShell not found";
    const int live = body.indexOf(QStringLiteral("shellPid()"));
    const int last = body.indexOf(QStringLiteral("lastShellPid()"));
    ASSERT_GE(live, 0);
    ASSERT_GE(last, 0) << "an exited shell (shellPid() == -1) is never released";
    EXPECT_LT(live, last) << "the live PID must be preferred over the recorded one";

    const QString header = readFile(SRC_TERMINALWIDGET_H_PATH);
    ASSERT_FALSE(header.isEmpty()) << "cannot read " << SRC_TERMINALWIDGET_H_PATH;
    EXPECT_TRUE(header.contains(QStringLiteral("pid_t lastShellPid() const")));

    const QString impl = readFile(SRC_TERMINALWIDGET_PATH);
    ASSERT_FALSE(impl.isEmpty()) << "cannot read " << SRC_TERMINALWIDGET_PATH;
    const QString start =
        functionBody(impl, QStringLiteral("bool TerminalWidget::startShell("));
    ASSERT_FALSE(start.isEmpty()) << "startShell not found";
    const int fail = start.indexOf(QStringLiteral("return false;"));
    const int set = start.indexOf(QStringLiteral("m_lastShellPid = ptyChildPid()"));
    ASSERT_GE(set, 0) << "startShell never records the shell PID";
    EXPECT_GT(set, fail) << "the PID is recorded before the start is known to succeed";
    // shellPid() keeps its -1-when-not-running meaning.
    const QString pidFn =
        functionBody(impl, QStringLiteral("pid_t TerminalWidget::shellPid() const"));
    EXPECT_TRUE(pidFn.contains(QStringLiteral("return ptyChildPid();")));
    EXPECT_FALSE(pidFn.contains(QStringLiteral("m_lastShellPid")));
}
