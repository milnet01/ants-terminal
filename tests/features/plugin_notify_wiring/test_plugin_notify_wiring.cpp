// ANTS-4273 — ants.notify() must reach the host, not stop at PluginManager.
// Spec: tests/features/plugin_notify_wiring/spec.md
//
// Source scrape: the defect is a missing signal connection, and constructing a
// MainWindow to observe one would need a live PTY, a tray and a plugin
// manager. Same pattern as shell_command_wiring and image_paste_uri_list
// INV-5.
#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTextStream>

#ifndef SRC_MAINWINDOW_CPP_PATH
#define SRC_MAINWINDOW_CPP_PATH ""
#endif
#ifndef PLUGINS_MD_PATH
#define PLUGINS_MD_PATH ""
#endif

namespace {

QString slurp(const char *path)
{
    const QString p = QString::fromUtf8(path);
    if (p.isEmpty()) {
        ADD_FAILURE() << "path define is empty — this test must run under CMake";
        return {};
    }
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ADD_FAILURE() << "cannot read " << path;
        return {};
    }
    QTextStream in(&f);
    return in.readAll();
}

}  // namespace

// INV-1 — the consumer that was missing. Without it the entire chain is inert.
TEST(PluginNotifyWiring, MainWindowConsumesPluginShowNotification)
{
    const QString src = slurp(SRC_MAINWINDOW_CPP_PATH);
    if (src.isEmpty()) return;

    EXPECT_TRUE(src.contains(QStringLiteral("PluginManager::showNotification")))
        << "ANTS-4273: MainWindow does not connect "
        << "PluginManager::showNotification, so ants.notify() is accepted and "
        << "does nothing — the signal stops at the plugin manager.";
}

// INV-2 — the documented status-bar fallback, used only when delivery failed.
TEST(PluginNotifyWiring, PluginPathFallsBackToTheStatusBar)
{
    const QString src = slurp(SRC_MAINWINDOW_CPP_PATH);
    if (src.isEmpty()) return;

    const int connectPos =
        src.indexOf(QStringLiteral("PluginManager::showNotification"));
    ASSERT_GT(connectPos, 0);

    // The lambda is short; the fallback must be inside it.
    const QString body = src.mid(connectPos, 600);
    EXPECT_TRUE(body.contains(QStringLiteral("showDesktopNotification")))
        << "ANTS-4273 INV-2/INV-3: the plugin connection must go through the "
        << "shared notification helper.";
    EXPECT_TRUE(body.contains(QStringLiteral("showStatusMessage")))
        << "ANTS-4273 INV-2: PLUGINS.md promises a fallback to the status bar "
        << "when the desktop notification does not land.";
    EXPECT_TRUE(body.contains(QStringLiteral("if (!showDesktopNotification")))
        << "ANTS-4273 INV-2: the status bar is the FALLBACK, not an addition — "
        << "two notifications for one ants.notify() is a different defect.";
}

// INV-3 — one implementation. A second copy is what this pins against.
TEST(PluginNotifyWiring, OneDesktopNotificationImplementation)
{
    const QString src = slurp(SRC_MAINWINDOW_CPP_PATH);
    if (src.isEmpty()) return;

    EXPECT_TRUE(src.contains(QStringLiteral("bool MainWindow::showDesktopNotification")))
        << "ANTS-4273 INV-3: the shared helper is gone — did a caller inline "
        << "the tray / notify-send logic again?";
    EXPECT_EQ(src.count(QStringLiteral("QSystemTrayIcon::isSystemTrayAvailable()")), 1)
        << "ANTS-4273 INV-3: the tray/notify-send logic appears more than once. "
        << "It was extracted from the OSC 9/777 lambda precisely so the plugin "
        << "path would not become a second copy.";
}

// INV-4 — the focus gate belongs to the OSC caller, not to the helper. Inside
// it, every plugin notification would be dropped whenever the window is
// focused, which PLUGINS.md does not say and a plugin author would not expect.
TEST(PluginNotifyWiring, FocusGateStaysOutsideTheSharedHelper)
{
    const QString src = slurp(SRC_MAINWINDOW_CPP_PATH);
    if (src.isEmpty()) return;

    const int helperPos =
        src.indexOf(QStringLiteral("bool MainWindow::showDesktopNotification"));
    ASSERT_GT(helperPos, 0);
    const int helperEnd = src.indexOf(QStringLiteral("\n}"), helperPos);
    ASSERT_GT(helperEnd, helperPos);

    EXPECT_FALSE(src.mid(helperPos, helperEnd - helperPos)
                     .contains(QStringLiteral("isActiveWindow")))
        << "ANTS-4273 INV-4: the focus gate moved into the shared helper, so "
        << "an explicit ants.notify() is now silently dropped whenever the "
        << "window is focused.";
}

// INV-5 — the documentation stopped being true when the wiring landed.
TEST(PluginNotifyWiring, PluginsMdNoLongerCallsNotifyANoOp)
{
    const QString doc = slurp(PLUGINS_MD_PATH);
    if (doc.isEmpty()) return;

    EXPECT_FALSE(doc.contains(QStringLiteral("`ants.notify()` is currently a no-op")))
        << "ANTS-4273 INV-5: PLUGINS.md still warns that ants.notify() does "
        << "nothing. It works now, and the warning tells plugin authors to "
        << "avoid a working function.";
}
