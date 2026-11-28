// Status probes and session commands stay tied to their tab — see spec.md.
// ANTS-5080. Source-scrape of the MainWindow sources.

#include <QFile>
#include <QString>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"


namespace {

QString source() {
    return QString::fromStdString(ants_test::slurpMainWindow());
}

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
TEST(MainwindowStatusProbes, ResumeQuotesSessionId) {
    const QString w = window(source(),
        QStringLiteral("&ClaudeProjectsDialog::resumeSession"), 700);
    ASSERT_FALSE(w.isEmpty()) << "resumeSession handler not found";
    EXPECT_TRUE(w.contains(QStringLiteral("shellQuote(sessionId)")));
}

// INV-2 is withdrawn: see spec.md.

// INV-3 and INV-4
TEST(MainwindowStatusProbes, ReviewProbeIsKeyedAndBounded) {
    const QString body = functionBody(source(),
        QStringLiteral("void MainWindow::refreshReviewButton()"));
    ASSERT_FALSE(body.isEmpty()) << "refreshReviewButton not found";
    EXPECT_TRUE(body.contains(QStringLiteral("m_reviewProbeCwd = cwd")));
    EXPECT_TRUE(body.contains(QStringLiteral("m_reviewProbeCwd != cwd")));
    EXPECT_TRUE(body.contains(QStringLiteral("stillCurrent")))
        << "a finished probe applies its result whichever tab is active";
    EXPECT_TRUE(body.contains(QStringLiteral("guard->kill()")))
        << "a probe that never exits holds the in-flight flag forever";
}

// INV-5
TEST(MainwindowStatusProbes, ReviewProbeErrorIsKeyedToo) {
    const QString body = functionBody(source(),
        QStringLiteral("void MainWindow::refreshReviewButton()"));
    ASSERT_FALSE(body.isEmpty()) << "refreshReviewButton not found";
    const int at = body.indexOf(QStringLiteral("&QProcess::errorOccurred"));
    ASSERT_GE(at, 0) << "the probe has no error handler";
    const QString handler = body.mid(at, 300);
    EXPECT_TRUE(handler.contains(QStringLiteral("if (btn && probedTabIsCurrent()) btn->hide();")))
        << "a failed probe hides the Review button on whichever tab is active";
    EXPECT_FALSE(handler.contains(QStringLiteral("if (btn) btn->hide();")));
}
