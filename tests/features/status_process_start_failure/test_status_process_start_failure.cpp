// A helper process that fails to start is cleaned up — see spec.md.
// ANTS-5080 / ANTS-5081. Source-scrape.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#define SRC_MAINWINDOW_CPP_PATH ""
#endif

namespace {

QString readText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString mainwindowSource() { return readText(QStringLiteral(SRC_MAINWINDOW_CPP_PATH)); }

QString trackerSource() {
    const QDir src = QFileInfo(QStringLiteral(SRC_MAINWINDOW_CPP_PATH)).dir();
    return readText(src.filePath(QStringLiteral("kwinpositiontracker.cpp")));
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

// INV-1 and INV-2
TEST(StatusProcessStartFailure, RepoVisibilityProbeRecovers) {
    const QString body = functionBody(mainwindowSource(),
        QStringLiteral("void MainWindow::refreshRepoVisibility("));
    ASSERT_FALSE(body.isEmpty()) << "refreshRepoVisibility not found";
    EXPECT_TRUE(body.contains(QStringLiteral("&QProcess::errorOccurred")))
        << "a gh that fails to start leaves the in-flight flag set";
    EXPECT_TRUE(body.contains(QStringLiteral("m_repoVisibilityProbeInFlight[repoRoot] = false")));
    EXPECT_TRUE(body.contains(QStringLiteral("proc->kill()")))
        << "a gh that never exits holds the in-flight flag forever";
}

// INV-3 and INV-4
TEST(StatusProcessStartFailure, KWinTrackerCleansUpBothStages) {
    const QString body = functionBody(trackerSource(),
        QStringLiteral("void KWinPositionTracker::setPosition("));
    ASSERT_FALSE(body.isEmpty()) << "setPosition not found";
    EXPECT_EQ(body.count(QStringLiteral("&QProcess::errorOccurred")), 2)
        << "both dbus-send stages need a failed-start handler";
    EXPECT_TRUE(body.contains(QStringLiteral("if (error == QProcess::FailedToStart) proc->deleteLater();")))
        << "the first stage leaks its QProcess when it fails to start";
}
