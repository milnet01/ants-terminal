// Main-window commands and exports fail safely — see spec.md. ANTS-5079.
// Source-scrape of src/mainwindow.cpp.

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

// The text from `anchor` to `span` characters after it.
QString window(const QString &src, const QString &anchor, int span) {
    const int at = src.indexOf(anchor);
    return at < 0 ? QString() : src.mid(at, span);
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
TEST(MainwindowCommandSafety, ReviewCommandQuotesItsPrompt) {
    const QString w = window(source(),
        QStringLiteral("&AuditDialog::reviewRequested"), 1400);
    ASSERT_FALSE(w.isEmpty()) << "reviewRequested handler not found";
    EXPECT_TRUE(w.contains(QStringLiteral("shellQuote(prompt)")));
    EXPECT_FALSE(w.contains(QStringLiteral("QString(\"claude \\\"Read %1")))
        << "the results path is still dropped inside hand-written quotes";
}

// INV-2
TEST(MainwindowCommandSafety, ScrollbackExportReportsFailure) {
    const QString w = window(source(),
        QStringLiteral("\"Export Scro&llback...\""), 1600);
    ASSERT_FALSE(w.isEmpty()) << "Export Scrollback handler not found";
    EXPECT_TRUE(w.contains(QStringLiteral("QSaveFile file(path)")));
    EXPECT_TRUE(w.contains(QStringLiteral("file.commit()")));
    EXPECT_TRUE(w.contains(QStringLiteral("failed")));
}

// INV-3
TEST(MainwindowCommandSafety, KWinScriptCleansUpWhenStartFails) {
    const QString body = functionBody(source(),
        QStringLiteral("void MainWindow::runKWinScript("));
    ASSERT_FALSE(body.isEmpty()) << "runKWinScript not found";
    EXPECT_EQ(body.count(QStringLiteral("&QProcess::errorOccurred")), 2)
        << "both dbus-send processes need a FailedToStart handler";
    EXPECT_TRUE(body.contains(QStringLiteral("QProcess::FailedToStart")));
}

// INV-4
TEST(MainwindowCommandSafety, SshTimerIsGuarded) {
    const QString body = functionBody(source(),
        QStringLiteral("void MainWindow::onSshConnect("));
    ASSERT_FALSE(body.isEmpty()) << "onSshConnect not found";
    EXPECT_TRUE(body.contains(QStringLiteral("QPointer<TerminalWidget>")));
    EXPECT_FALSE(body.contains(QStringLiteral("[t, sshCommand]")))
        << "the SSH timer still captures a raw terminal pointer";
}
