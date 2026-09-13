// Status probes and session commands stay tied to their tab — see spec.md.
// ANTS-5080. Source-scrape of src/mainwindow.cpp.

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

// INV-2
TEST(MainwindowStatusProbes, ReaperChecksForASocket) {
    const QString w = window(source(),
        QStringLiteral("Reaped stale MCP socket:"), 1);
    ASSERT_FALSE(w.isEmpty()) << "socket reaper not found";
    const QString src = source();
    const int log = src.indexOf(QStringLiteral("Reaped stale MCP socket:"));
    const QString before = src.mid(qMax(0, log - 600), 600);
    EXPECT_TRUE(before.contains(QStringLiteral("safeToUnlinkLocalSocket(full)")))
        << "the reaper removes the path without the S_ISSOCK check its "
           "comment promises";
}

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
