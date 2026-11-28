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

// INV-6
TEST(MainwindowCommandSafety, WriteCommandGetsNoSecondEnter) {
    const QString undo = window(source(), QStringLiteral("\"&Undo Close Tab\""), 900);
    ASSERT_FALSE(undo.isEmpty()) << "Undo Close Tab handler not found";
    EXPECT_TRUE(undo.contains(QStringLiteral("writeCommand(\"cd \" + shellQuote(info.cwd));")))
        << "the restored tab's cd is sent with a second Enter";
    const QString review = window(source(),
        QStringLiteral("&AuditDialog::reviewRequested"), 1400);
    ASSERT_FALSE(review.isEmpty()) << "reviewRequested handler not found";
    EXPECT_TRUE(review.contains(QStringLiteral("t->writeCommand(claudeLaunch(")))
        << "the review command is sent with a second Enter into Claude";
}

// INV-7
TEST(MainwindowCommandSafety, SnippetButtonsActOnTheShownSnippet) {
    const std::string body = ants_test::slurpFunctionBody(
        ants_test::slurpMainWindow(), "void MainWindow::showSnippetsDialog()");
    ASSERT_FALSE(body.empty()) << "showSnippetsDialog not found";
    const QString b = QString::fromStdString(body);
    EXPECT_TRUE(b.contains(QStringLiteral("setData(Qt::UserRole")))
        << "a table row does not record which snippet it shows";
    EXPECT_EQ(b.count(QStringLiteral("table->currentRow()")), 1)
        << "a button reads the filtered row number as an index into the snippets";
    EXPECT_EQ(b.count(QStringLiteral("loadSnippets(searchEdit->text())")), 2)
        << "an edit or delete reloads the table unfiltered while the search "
           "text still shows";
}
