// Main-window commands and exports fail safely — see spec.md. ANTS-5079.
// Source-scrape of the MainWindow sources.

#include <QFile>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"


namespace {

QString source() {
    return QString::fromStdString(ants_test::slurpMainWindow());
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
    EXPECT_TRUE(w.contains(QStringLiteral("startExport(")));
    EXPECT_TRUE(w.contains(QStringLiteral("&TerminalWidget::exportFinished")));
    EXPECT_TRUE(w.contains(QStringLiteral("if (ok) showStatusMessage(\"Scrollback exported to")))
        << "success is announced before the export has been written";
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

// INV-5
TEST(MainwindowCommandSafety, ProgressIconRebuiltOnlyOnStateChange) {
    const QString w = window(source(),
        QStringLiteral("&TerminalWidget::progressChanged"), 1600);
    ASSERT_FALSE(w.isEmpty()) << "progressChanged handler not found";
    const int record = w.indexOf(QStringLiteral("page->property(\"antsProgressIconState\")"));
    const int paint = w.indexOf(QStringLiteral("QPixmap pm("));
    ASSERT_GE(record, 0) << "the handler does not record the drawn state on the tab page";
    ASSERT_GE(paint, 0) << "the handler no longer paints the icon";
    EXPECT_LT(record, paint) << "the state check comes after the icon is painted";
    EXPECT_TRUE(w.contains(QStringLiteral("drawn.toInt() == state) return;")));
}
