// The AppImage puts its own menu entry and icons in the user's data
// directory, so the panel and task manager show its icon.
//
// Contract:
//   INV-1  rewriteEntry points every Exec=ants-terminal line at the
//          AppImage, quoted, with % doubled, keeping its arguments; TryExec
//          names the AppImage; every other line is unchanged.
//   INV-2  rewriteEntry returns empty for a path holding " ` $ \ or a
//          newline.
//   INV-3  integrate writes the rewritten entry and each bundled PNG icon
//          under dataHome, and leaves an unchanged file's mtime alone.
//   INV-4  integrate returns false, writing nothing, when the bundle has no
//          menu entry.
//   INV-5  main() calls integrateFromEnvironment.

#include "appimagedesktop.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

ANTS_TEST_SCOPE();

namespace {

const QByteArray kEntry =
    "[Desktop Entry]\n"
    "Name=Ants Terminal\n"
    "Exec=ants-terminal\n"
    "TryExec=ants-terminal\n"
    "Icon=ants-terminal\n"
    "\n"
    "[Desktop Action QuakeMode]\n"
    "Exec=ants-terminal --quake\n";

void put(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << qPrintable(path);
    f.write(data);
}

QByteArray get(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

TEST(AppImageDesktop, Inv1RewriteEntry) {
    const QByteArray out = AppImageDesktop::rewriteEntry(
        kEntry, QStringLiteral("/home/u/My Apps/Ants 100%.AppImage"));
    EXPECT_EQ(out,
        QByteArray("[Desktop Entry]\n"
                   "Name=Ants Terminal\n"
                   "Exec=\"/home/u/My Apps/Ants 100%%.AppImage\"\n"
                   "TryExec=/home/u/My Apps/Ants 100%.AppImage\n"
                   "Icon=ants-terminal\n"
                   "\n"
                   "[Desktop Action QuakeMode]\n"
                   "Exec=\"/home/u/My Apps/Ants 100%%.AppImage\" --quake\n"));
    // A different program whose name starts the same is not ours.
    EXPECT_EQ(AppImageDesktop::rewriteEntry("Exec=ants-terminal-helper\n",
                                            QStringLiteral("/a.AppImage")),
              QByteArray("Exec=ants-terminal-helper\n"));
}

TEST(AppImageDesktop, Inv2RefusesAwkwardPaths) {
    for (const char *p : {"/a\"b", "/a`b", "/a$b", "/a\\b", "/a\nb"})
        EXPECT_TRUE(AppImageDesktop::rewriteEntry(kEntry, QString::fromUtf8(p)).isEmpty())
            << p;
}

TEST(AppImageDesktop, Inv3WritesEntryAndIcons) {
    QTemporaryDir appDir, home;
    ASSERT_TRUE(appDir.isValid() && home.isValid());
    const QString a = appDir.path(), h = home.path();
    put(a + "/usr/share/applications/" + AppImageDesktop::kEntryName, kEntry);
    put(a + "/usr/share/icons/hicolor/48x48/apps/ants-terminal.png", "png48");
    put(a + "/usr/share/icons/hicolor/256x256/apps/ants-terminal.png", "png256");
    QDir().mkpath(a + "/usr/share/icons/hicolor/scalable/apps");

    ASSERT_TRUE(AppImageDesktop::integrate(a, "/x/Ants.AppImage", h));
    const QString entryPath = h + "/applications/" + AppImageDesktop::kEntryName;
    EXPECT_EQ(get(entryPath), AppImageDesktop::rewriteEntry(kEntry, "/x/Ants.AppImage"));
    EXPECT_EQ(get(h + "/icons/hicolor/48x48/apps/ants-terminal.png"), "png48");
    EXPECT_EQ(get(h + "/icons/hicolor/256x256/apps/ants-terminal.png"), "png256");
    EXPECT_FALSE(QFileInfo::exists(h + "/icons/hicolor/scalable"));

    const QDateTime before = QFileInfo(entryPath).lastModified();
    QThread::msleep(20);
    ASSERT_TRUE(AppImageDesktop::integrate(a, "/x/Ants.AppImage", h));
    EXPECT_EQ(QFileInfo(entryPath).lastModified(), before) << "unchanged file rewritten";

    // A moved AppImage refreshes the entry.
    ASSERT_TRUE(AppImageDesktop::integrate(a, "/y/Ants.AppImage", h));
    EXPECT_TRUE(get(entryPath).contains("Exec=\"/y/Ants.AppImage\""));
}

TEST(AppImageDesktop, Inv4NoBundledEntry) {
    QTemporaryDir appDir, home;
    ASSERT_TRUE(appDir.isValid() && home.isValid());
    put(appDir.path() + "/usr/share/icons/hicolor/48x48/apps/ants-terminal.png", "p");
    EXPECT_FALSE(AppImageDesktop::integrate(appDir.path(), "/x.AppImage", home.path()));
    EXPECT_TRUE(QDir(home.path()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot));
}

TEST(AppImageDesktop, Inv5MainIntegrates) {
    QFile f(QStringLiteral(SRC_MAIN_CPP_PATH));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "read src/main.cpp";
    EXPECT_TRUE(f.readAll().contains("AppImageDesktop::integrateFromEnvironment()"));
}
