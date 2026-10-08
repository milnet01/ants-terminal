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
#include <QThread>
#include <QUuid>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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
    const QString mw  = QString::fromStdString(ants_test::slurpMainWindow());

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

// ANTS-5107 — teardown (INV-4). Each case is its own TEST, so it runs in its
// own process and a wedged worker from one cannot slow another.
namespace {

QString makeTeardownDir() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-lua-teardown-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(dir + QStringLiteral("/plugins"));
    return dir;
}

void addPlugin(const QString &dir, const QString &name, const QByteArray &init,
               bool settingsPermission = false) {
    const QString p = dir + QStringLiteral("/plugins/") + name;
    writeTemp(p, QStringLiteral("init.lua"), init);
    if (settingsPermission)
        writeTemp(p, QStringLiteral("manifest.json"),
                  R"({"permissions":["settings"]})");
}

struct TeardownResult {
    qint64 ms = 0;
    int detached = 0;
    int settingsSets = 0;
};

// Loads `names`, fires `fires` CommandFinished events, then parks the GUI
// thread for `parkMs` WITHOUT pumping its loop, so a worker blocked on a
// call to this thread stays blocked. Then times the destructor.
TeardownResult timeTeardown(const QString &dir, const QStringList &names,
                            int fires, int parkMs) {
    TeardownResult r;
    QElapsedTimer t;
    {
        PluginManager pm;
        pm.setGrantStore(
            [](const QString &) { return QStringList{QStringLiteral("settings")}; },
            [](const QString &, const QStringList &) {});
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&r](const QString &m) {
            if (m.contains(QStringLiteral("detached"))) ++r.detached;
        });
        QObject::connect(&pm, &PluginManager::settingsSetRequested,
                         [&r](const QString &, const QString &, const QString &,
                              QString &) { ++r.settingsSets; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad(names);
        QElapsedTimer warm; warm.start();
        while (warm.elapsed() < 300)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        for (int i = 0; i < fires; ++i)
            pm.fireEvent(PluginEvent::CommandFinished,
                         QStringLiteral("exit_code=0&duration_ms=1"));
        QThread::msleep(parkMs);
        t.start();
    }
    r.ms = t.elapsed();
    QDir(dir).removeRecursively();
    return r;
}

}  // namespace

// A worker already blocked in settings.get when teardown starts waits on the
// GUI thread; teardown must answer it rather than sit out the deadline.
TEST(LuaThreading, Ants5107TeardownAnswersAPendingSettingsGet) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("asker"),
              "ants.on('command_finished', function() ants.settings.get('k') end)\n",
              true);
    const TeardownResult r =
        timeTeardown(dir, {QStringLiteral("asker")}, 1, 300);
    EXPECT_EQ(r.detached, 0) << "a healthy plugin was detached as a zombie";
    EXPECT_LT(r.ms, 1000) << "teardown took " << r.ms << " ms";
}

// Unload is for saving state, so its settings.set must reach the store.
TEST(LuaThreading, Ants5107UnloadSettingsSetIsSaved) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("saver"),
              "ants.on('unload', function() ants.settings.set('k', 'v') end)\n",
              true);
    const TeardownResult r =
        timeTeardown(dir, {QStringLiteral("saver")}, 0, 0);
    EXPECT_EQ(r.settingsSets, 1) << "Unload's settings.set never arrived";
    EXPECT_EQ(r.detached, 0) << "a healthy plugin was detached as a zombie";
    EXPECT_LT(r.ms, 1000) << "teardown took " << r.ms << " ms";
}

// Queued events must not hold Unload back past the deadline. Each handler
// is a runaway that the 1.5 s budget would end, so the plugin is healthy.
TEST(LuaThreading, Ants5107BacklogDoesNotZombifyAHealthyPlugin) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("busy"),
              "ants.on('command_finished', function() while true do end end)\n");
    const TeardownResult r =
        timeTeardown(dir, {QStringLiteral("busy")}, 3, 200);
    EXPECT_EQ(r.detached, 0) << "a healthy plugin was detached as a zombie";
    EXPECT_LT(r.ms, 1000) << "teardown took " << r.ms << " ms";
}

// Two workers stuck in one uninterruptible C call (a pattern match the
// instruction hook cannot interrupt) share one deadline, not one each.
TEST(LuaThreading, Ants5107WedgedWorkersShareOneDeadline) {
    const QString dir = makeTeardownDir();
    const QByteArray wedge =
        "ants.on('command_finished', function()\n"
        "  string.find(string.rep('a', 3000), '.-.-.-.-b')\n"
        "end)\n";
    addPlugin(dir, QStringLiteral("wedge1"), wedge);
    addPlugin(dir, QStringLiteral("wedge2"), wedge);
    const TeardownResult r = timeTeardown(
        dir, {QStringLiteral("wedge1"), QStringLiteral("wedge2")}, 1, 300);
    EXPECT_EQ(r.detached, 2) << "both stuck workers must be detached";
    EXPECT_LT(r.ms, 2700) << "teardown took " << r.ms
                          << " ms; the deadline is 2000 ms in total";
}

// ANTS-5107 — the health check (INV-3). Tests call healthTick by name rather
// than wait on its 2 s timer.
namespace {

void pump(int ms) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void runHealthTick(PluginManager &pm) {
    QMetaObject::invokeMethod(&pm, "healthTick", Qt::DirectConnection);
}

// Loads `name` from `dir`, hands the manager to `drive`, and returns how many
// demotions the manager logged.
template <class Drive>
int countDemotions(const QString &dir, const QString &name, Drive drive) {
    int demoted = 0;
    {
        PluginManager pm;
        pm.setGrantStore(
            [](const QString &) { return QStringList{QStringLiteral("settings")}; },
            [](const QString &, const QStringList &) {});
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&demoted](const QString &m) {
            if (m.contains(QStringLiteral("demoted"))) ++demoted;
        });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({name});
        drive(pm);
    }
    QDir(dir).removeRecursively();
    return demoted;
}

}  // namespace

// The engine's budget restarts for each handler, so the health check must
// time each handler, not the whole event. Each of three handlers blocks
// 900 ms in settings.get, and the check runs while it is blocked: on the
// third, about 2.7 s into the event and past budget + grace (2.5 s).
TEST(LuaThreading, Ants5107HealthTimesEachHandler) {
    const QString dir = makeTeardownDir();
    QByteArray init;
    for (int i = 0; i < 3; ++i)
        init += "ants.on('command_finished', function() ants.settings.get('k') end)\n";
    addPlugin(dir, QStringLiteral("threeslow"), init, true);
    const int demoted = countDemotions(dir, QStringLiteral("threeslow"),
                                       [](PluginManager &pm) {
        QObject::connect(&pm, &PluginManager::settingsGetRequested,
                         [&pm](const QString &, const QString &, QString &) {
            QThread::msleep(900);
            runHealthTick(pm);
        });
        pump(300);
        pm.fireEvent(PluginEvent::CommandFinished,
                     QStringLiteral("exit_code=0&duration_ms=1"));
        pump(3500);
    });
    EXPECT_EQ(demoted, 0) << "a plugin whose handlers each finish in budget "
                             "was demoted";
}

// A plugin stuck in one uninterruptible C call while init.lua runs is as
// unresponsive as one stuck in a handler, so the health check demotes it.
TEST(LuaThreading, Ants5107HealthSeesAWedgedInitLua) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("wedgedinit"),
              "string.find(string.rep('a', 3000), '.-.-.-.-b')\n");
    const int demoted = countDemotions(dir, QStringLiteral("wedgedinit"),
                                       [](PluginManager &pm) {
        QElapsedTimer t; t.start();
        while (t.elapsed() < 3200) {
            pump(100);
            runHealthTick(pm);
        }
    });
    EXPECT_EQ(demoted, 1) << "a plugin stuck in init.lua was never demoted";
}

// Time a handler spends blocked in settings.get is the GUI thread's time,
// not the plugin's, so it does not count against the 1.5 s handler budget.
TEST(LuaThreading, Ants5107SettingsWaitIsNotBudgetTime) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("patient"),
              "ants.on('command_finished', function()\n"
              "  ants.settings.get('k')\n"
              "  for i = 1, 200000 do end\n"
              "  ants.log('finished')\n"
              "end)\n",
              true);
    bool finished = false;
    {
        PluginManager pm;
        pm.setGrantStore(
            [](const QString &) { return QStringList{QStringLiteral("settings")}; },
            [](const QString &, const QStringList &) {});
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&finished](const QString &m) {
            if (m == QStringLiteral("finished")) finished = true;
        });
        QObject::connect(&pm, &PluginManager::settingsGetRequested,
                         [](const QString &, const QString &, QString &) {
            QThread::msleep(1700);
        });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("patient")});
        pump(300);
        pm.fireEvent(PluginEvent::CommandFinished,
                     QStringLiteral("exit_code=0&duration_ms=1"));
        pump(2500);
    }
    QDir(dir).removeRecursively();
    EXPECT_TRUE(finished) << "the handler was stopped for time it spent "
                             "waiting on the GUI thread";
}

// Saving a plugin several times in quick succession reloads it once.
TEST(LuaThreading, Ants5107DevReloadRunsOncePerBurst) {
    qputenv("ANTS_PLUGIN_DEV", "1");
    if (!PluginManager::devMode())
        GTEST_SKIP() << "dev mode was read before this test could set it";
    const QString dir = makeTeardownDir();
    const QString pluginDir = dir + QStringLiteral("/plugins/edited");
    addPlugin(dir, QStringLiteral("edited"), "ants.on('load', function() end)\n");
    int reloads = 0;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::pluginsReloaded,
                         [&reloads]() { ++reloads; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("edited")});
        pump(300);
        reloads = 0;
        for (int i = 0; i < 4; ++i) {
            writeTemp(pluginDir, QStringLiteral("init.lua"),
                      "-- edit " + QByteArray::number(i)
                          + "\nants.on('load', function() end)\n");
            pump(50);
        }
        pump(1000);
    }
    QDir(dir).removeRecursively();
    EXPECT_EQ(reloads, 1) << "four quick saves reloaded the plugin "
                          << reloads << " times";
}
