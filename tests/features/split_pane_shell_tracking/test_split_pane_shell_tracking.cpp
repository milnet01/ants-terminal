// A split pane's shell is tracked and released like a tab's — see spec.md.
// ANTS-5079. Source-scrape of src/mainwindow.cpp.

#include <QFile>
#include <QString>

#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#define SRC_MAINWINDOW_CPP_PATH ""
#endif

namespace {

QString source() {
    QFile f(QStringLiteral(SRC_MAINWINDOW_CPP_PATH));
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
