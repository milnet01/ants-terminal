// Feature-conformance test for spec.md (ANTS-1750) —
//
// Runtime:
//   R1 — PluginManager::fireEvent does NOT block the GUI thread while a
//        plugin handler runs a long busy loop (pre-fix it runs the
//        handler inline → blocks for the handler's full runtime).
// Source (each fails on pre-fix source — the symbols don't exist yet):
//   S1 — worker affinity: pluginmanager moveToThread( + LuaEngine
//        dispatchEvent slot.
//   S2 — parentless engine: new LuaEngine(nullptr).
//   S3 — GUI-set abort: std::atomic<bool> m_abortRequested, read in hook.
//   S4 — health bracketing: eventStarted/eventCompleted signals;
//        dispatchTo / healthyEngines / healthTick / m_zombies.
//   S5 — settings.get blocks the worker: BlockingQueuedConnection.
//   S6 — no lua_close on the GUI thread: no engine->shutdown() in
//        pluginmanager.cpp.
//   S7 — targeted dispatch off the GUI thread: MainWindow keybinding
//        routes through dispatchTo.
//
// Links against src/luaengine.cpp + src/pluginmanager.cpp + Lua 5.4.
// Drives a real PluginManager (no GUI). Pre-fix code blocks the GUI
// thread in R1; the bundle's CTest TIMEOUT backstops any hang.

#include "luaengine.h"
#include "pluginmanager.h"

#include "../../_support/expect.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

QString writeTemp(const QString &dir, const QString &basename,
                  const QByteArray &bytes) {
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/") + basename;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        expect(false, "setup/writeTemp",
               QStringLiteral("cannot write %1").arg(path));
        return {};
    }
    f.write(bytes);
    f.close();
    return path;
}

QString readSource(const char *macroPath, const char *label) {
    QFile f(QString::fromUtf8(macroPath));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        expect(false, label,
               QStringLiteral("cannot read %1").arg(QString::fromUtf8(macroPath)));
        return {};
    }
    const QString s = QString::fromUtf8(f.readAll());
    f.close();
    return s;
}

// R1 — fireEvent must not block the GUI thread. A plugin handler runs a
// long pure-Lua busy loop; pre-fix PluginManager::fireEvent runs it
// inline and blocks for the loop's full runtime, post-fix it posts to a
// worker and returns immediately. The default 1.5 s wall budget does
// not kill the loop (it targets a few hundred ms), so pre-fix the block
// is clearly above the threshold; the margin is wide so the absolute
// iteration→ms calibration need not be precise.
void runR1(const QString &baseDir) {
    const QString pdir = baseDir + QStringLiteral("/plugins");
    const QString freezer = pdir + QStringLiteral("/freezer");
    QDir().mkpath(freezer);
    const QByteArray init =
        "ants.on('command_finished', function(d)\n"
        "  local x = 0\n"
        "  for i = 1, 150000000 do x = x + 1 end\n"
        "  return x\n"
        "end)\n";
    if (writeTemp(freezer, QStringLiteral("init.lua"), init).isEmpty()) return;

    PluginManager pm;
    pm.setPluginDir(pdir);
    pm.scanAndLoad(QStringList() << QStringLiteral("freezer"));

    expect(pm.engineFor(QStringLiteral("freezer")) != nullptr,
           "R1a/plugin-loaded",
           QStringLiteral("freezer plugin did not load"));

    // Post-fix the worker registers its handler asynchronously; pump the
    // loop briefly so the load queue (initialize→loadScript→Load) drains
    // before we fire. Pre-fix this is a no-op (load was synchronous).
    QElapsedTimer warm; warm.start();
    while (warm.elapsed() < 200)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    QElapsedTimer t; t.start();
    pm.fireEvent(PluginEvent::CommandFinished,
                 QStringLiteral("exit_code=0&duration_ms=1"));
    const qint64 fireMs = t.elapsed();

    expect(fireMs <= 150,
           "R1b/fireEvent-nonblocking",
           QStringLiteral("PluginManager::fireEvent took %1ms — it must not "
                          "block the GUI thread on the handler (pre-fix runs "
                          "the handler inline)").arg(fireMs));

    // Drain so a post-fix worker finishes before teardown (keeps the
    // detach path out of this test's noise).
    QElapsedTimer drain; drain.start();
    while (drain.elapsed() < 1500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void runSourceChecks() {
    const QString lh  = readSource(SRC_LUAENGINE_H_PATH, "src-open/luaengine.h");
    const QString lc  = readSource(SRC_LUAENGINE_PATH,   "src-open/luaengine.cpp");
    const QString pmh = readSource(SRC_PLUGINMANAGER_H_PATH, "src-open/pluginmanager.h");
    const QString pmc = readSource(SRC_PLUGINMANAGER_CPP_PATH, "src-open/pluginmanager.cpp");
    const QString mw  = readSource(SRC_MAINWINDOW_PATH,  "src-open/mainwindow.cpp");

    // S1 — worker affinity.
    expect(pmc.contains(QStringLiteral("moveToThread(")),
           "S1a/moveToThread",
           QStringLiteral("pluginmanager.cpp must moveToThread each engine"));
    expect(lh.contains(QStringLiteral("dispatchEvent")),
           "S1b/dispatchEvent-slot",
           QStringLiteral("LuaEngine must expose a worker-side dispatchEvent slot"));

    // S2 — parentless engine (a parented QObject cannot moveToThread).
    expect(pmc.contains(QStringLiteral("new LuaEngine(nullptr)")),
           "S2/parentless-engine",
           QStringLiteral("loadPlugin must construct new LuaEngine(nullptr)"));

    // S3 — GUI-set abort flag, read in the hook.
    expect(lh.contains(QStringLiteral("std::atomic<bool> m_abortRequested")),
           "S3a/abort-atomic-declared",
           QStringLiteral("LuaEngine must declare std::atomic<bool> m_abortRequested"));
    expect(lc.contains(QStringLiteral("m_abortRequested")),
           "S3b/abort-read-in-hook",
           QStringLiteral("instructionHook must read m_abortRequested"));

    // S4 — execution-time health bracketing + controller surface.
    expect(lh.contains(QStringLiteral("eventStarted")) &&
               lh.contains(QStringLiteral("eventCompleted")),
           "S4a/started-completed-signals",
           QStringLiteral("LuaEngine must declare eventStarted + eventCompleted"));
    expect(pmh.contains(QStringLiteral("dispatchTo")),
           "S4b/dispatchTo",
           QStringLiteral("PluginManager must declare dispatchTo"));
    expect(pmh.contains(QStringLiteral("healthyEngines")),
           "S4c/healthyEngines",
           QStringLiteral("PluginManager must declare healthyEngines"));
    expect(pmh.contains(QStringLiteral("healthTick")),
           "S4d/healthTick",
           QStringLiteral("PluginManager must declare a healthTick slot"));
    expect(pmh.contains(QStringLiteral("m_zombies")),
           "S4e/zombie-list",
           QStringLiteral("PluginManager must declare an m_zombies detach list"));

    // S5 — settings.get blocks the worker, not the GUI.
    expect(pmc.contains(QStringLiteral("BlockingQueuedConnection")),
           "S5/settings-get-blocking-queued",
           QStringLiteral("wireEngine must use Qt::BlockingQueuedConnection "
                          "for the settingsGetRequested edge"));

    // S5b (ANTS-1997 / ANTS-2117) — teardown severs the blocking settings.get
    // edge BEFORE posting Unload. The Unload handler ("save state") may call
    // ants.settings.get, whose blocking-queued emit would park the worker
    // while the GUI thread is in thread->wait() (not spinning its loop) — a
    // 2 s stall that spuriously zombifies a healthy plugin. With the edge
    // severed first, a late settings.get finds no slot and returns nil.
    {
        const int tdIdx =
            pmc.indexOf(QStringLiteral("PluginManager::teardownEngine"));
        const int discIdx = pmc.indexOf(
            QStringLiteral("disconnect(engine, &LuaEngine::settingsGetRequested"),
            tdIdx);
        const int unloadIdx =
            pmc.indexOf(QStringLiteral("PluginEvent::Unload"), tdIdx);
        expect(tdIdx >= 0 && discIdx > tdIdx && unloadIdx > discIdx,
               "S5b/sever-settings-edge-before-unload",
               QStringLiteral("teardownEngine must disconnect "
                              "settingsGetRequested before posting Unload "
                              "(td=%1 disc=%2 unload=%3)")
                   .arg(tdIdx).arg(discIdx).arg(unloadIdx));
    }

    // S6 — no lua_close on the GUI thread: the synchronous GUI-path
    // engine->shutdown() calls (unloadAll + load-failure) are gone.
    expect(!pmc.contains(QStringLiteral("engine->shutdown()")),
           "S6/no-gui-path-shutdown",
           QStringLiteral("pluginmanager.cpp must not call engine->shutdown() "
                          "on the GUI thread (teardown via deleteLater posted "
                          "to the worker)"));

    // S7 — targeted dispatch off the GUI thread.
    expect(mw.contains(QStringLiteral("dispatchTo")),
           "S7/keybinding-via-dispatchTo",
           QStringLiteral("MainWindow keybinding shortcut must route through "
                          "PluginManager::dispatchTo, not engineFor()->fireEvent()"));
}

int runMain() {
    expect_reset();
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-lua-threading-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(dir);
    runR1(dir);
    runSourceChecks();
    QDir(dir).removeRecursively();
    return expect_finish();
}

}  // namespace

TEST(LuaThreading, Main) {
    ASSERT_EQ(0, runMain());
}
