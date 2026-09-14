// The About dialog describes the current renderer — see spec.md. ANTS-5082.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#define SRC_MAINWINDOW_CPP_PATH ""
#endif

TEST(AboutTextCurrent, NoGpuRenderingClaim) {
    const QDir src = QFileInfo(QStringLiteral(SRC_MAINWINDOW_CPP_PATH)).dir();
    QFile f(src.filePath(QStringLiteral("aboutdialogs.cpp")));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString code = QString::fromUtf8(f.readAll());
    const int s = code.indexOf(QStringLiteral("void showAboutAnts("));
    ASSERT_GE(s, 0) << "showAboutAnts not found";
    const QString body = code.mid(s, 2500);
    EXPECT_FALSE(body.contains(QStringLiteral("GPU")))
        << "the About text still advertises GPU rendering";
}
