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
#include "../../_support/xdg_guard.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QThread>
#include <QUuid>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <lua.hpp>
#include "../../_support/srcgrep.h"

ANTS_TEST_SCOPE();

namespace {

QString writeTemp(const QString &dir, const QString &basename,
                  const QByteArray &bytes) {
    QDir().mkpath(dir);
    QString path = dir + QStringLiteral("/") + basename;
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
        // ANTS-6283 — on a loaded host the plugin thread starts late, so the
        // stall passes budget + grace well after any fixed window. Wait for
        // the demotion, then tick a little longer so a second one would show.
        // `ctx` cuts the connection before `seen` goes out of scope.
        QObject ctx;
        bool seen = false;
        QObject::connect(&pm, &PluginManager::logMessage, &ctx,
                         [&seen](const QString &m) {
            if (m.contains(QStringLiteral("demoted"))) seen = true;
        });
        QElapsedTimer t; t.start();
        while (!seen && t.elapsed() < 15000) {
            pump(100);
            runHealthTick(pm);
        }
        for (int i = 0; i < 5; ++i) {
            pump(100);
            runHealthTick(pm);
        }
    });
    EXPECT_EQ(demoted, 1) << "a plugin stuck in init.lua must be demoted "
                             "exactly once";
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
    ants_test::XdgGuard env;
    env.setEnv("ANTS_PLUGIN_DEV", "1");
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

// A plugin whose worker had to be abandoned is not loaded again until its
// files change, so reloading does not leave another stuck worker each time.
TEST(LuaThreading, Ants5107WedgedPluginReloadsOnlyWhenChanged) {
    const QString dir = makeTeardownDir();
    const QString pluginDir = dir + QStringLiteral("/plugins/stuck");
    addPlugin(dir, QStringLiteral("stuck"),
              "string.find(string.rep('a', 3000), '.-.-.-.-b')\n");
    const QStringList enabled{QStringLiteral("stuck")};
    int detached = 0;
    bool reloadedUnchanged = false;
    bool fixedRan = false;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&](const QString &m) {
            if (m.contains(QStringLiteral("detached"))) ++detached;
            if (m == QStringLiteral("fixed")) fixedRan = true;
        });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad(enabled);
        pump(500);
        pm.reloadAll(enabled);
        reloadedUnchanged = pm.engineFor(QStringLiteral("stuck")) != nullptr;
        pump(500);  // long enough for a reloaded copy to wedge again
        pm.reloadAll(enabled);
        writeTemp(pluginDir, QStringLiteral("init.lua"), "ants.log('fixed')\n");
        pm.reloadAll(enabled);
        pump(300);
    }
    QDir(dir).removeRecursively();
    EXPECT_FALSE(reloadedUnchanged) << "an unchanged wedged plugin was loaded again";
    EXPECT_EQ(detached, 1) << "each reload left another stuck worker";
    EXPECT_TRUE(fixedRan) << "the plugin was not loaded after its files changed";
}

// ANTS-5107 — what a plugin sends the GUI thread. Logs and notifications
// past a per-second cap are dropped and reported once; status text and
// palette entries keep only the latest; a broadcast skips a plugin with no
// handler for it.
TEST(LuaThreading, Ants5107LogAndNotifyFloodsAreCapped) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("chatty"),
              "ants.on('load', function()\n"
              "  for i = 1, 1000 do ants.log('flood') end\n"
              "  for i = 1, 50 do ants.notify('t', 'm') end\n"
              "end)\n");
    int logs = 0;
    int notes = 0;
    QStringList reports;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&](const QString &m) {
            if (m == QStringLiteral("flood")) ++logs;
            if (m.contains(QStringLiteral("dropped"))) reports << m;
        });
        QObject::connect(&pm, &PluginManager::showNotification,
                         [&notes](const QString &, const QString &) { ++notes; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("chatty")});
        pump(500);
        runHealthTick(pm);
    }
    QDir(dir).removeRecursively();
    EXPECT_LE(logs, 2 * LuaEngine::kMaxLogsPerSecond) << logs << " logs reached the GUI";
    EXPECT_LE(notes, 2 * LuaEngine::kMaxNotificationsPerSecond)
        << notes << " notifications reached the GUI";
    ASSERT_EQ(reports.size(), 1) << "expected one report of what was dropped";
    EXPECT_TRUE(reports.first().contains(
        QStringLiteral("dropped %1 log and %2 notification")
            .arg(1000 - logs).arg(50 - notes)))
        << qPrintable(reports.first());
}

TEST(LuaThreading, Ants5107StatusAndPaletteKeepTheLatest) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("busybar"),
              "ants.on('load', function()\n"
              "  for i = 1, 1000 do\n"
              "    ants.set_status('s' .. i)\n"
              "    ants.palette.register({title = 'T', action = 'a',\n"
              "                           hotkey = 'F' .. (i % 12 + 1)})\n"
              "  end\n"
              "end)\n");
    QStringList statuses;
    QStringList hotkeys;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::statusMessage,
                         [&statuses](const QString &s) { statuses << s; });
        QObject::connect(&pm, &PluginManager::paletteEntryRegistered,
                         [&hotkeys](const QString &, const QString &,
                                    const QString &, const QString &h) { hotkeys << h; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("busybar")});
        // The GUI thread does not run its loop while the plugin floods, so
        // everything the plugin sent is waiting when it next does.
        QThread::msleep(500);
        pump(300);
    }
    QDir(dir).removeRecursively();
    EXPECT_EQ(statuses.size(), 1) << statuses.size() << " status updates reached the GUI";
    EXPECT_EQ(hotkeys.size(), 1) << hotkeys.size() << " palette updates reached the GUI";
    ASSERT_FALSE(statuses.isEmpty());
    ASSERT_FALSE(hotkeys.isEmpty());
    EXPECT_EQ(statuses.last(), QStringLiteral("s1000"));
    EXPECT_EQ(hotkeys.last(), QStringLiteral("F5"));  // 1000 % 12 + 1
}

// ANTS-6506 — a plugin holds at most kMaxPaletteEntries distinct palette
// entries. The next distinct one raises a Lua error; re-registering one it
// already holds still replaces it.
TEST(LuaThreading, Ants6506PaletteEntriesPerPluginAreCapped) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("hoarder"),
              "ants.on('load', function()\n"
              "  for i = 1, 200 do\n"
              "    local ok, err = pcall(ants.palette.register,\n"
              "                          {title = 'T' .. i, action = 'a' .. i})\n"
              "    if not ok then ants.log('refused at ' .. i .. ': ' .. tostring(err)); break end\n"
              "  end\n"
              "  local ok = pcall(ants.palette.register, {title = 'T1', action = 'a1', hotkey = 'F2'})\n"
              "  ants.log('replace ' .. tostring(ok))\n"
              "end)\n");
    QSet<QString> entries;
    QStringList logs;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::paletteEntryRegistered,
                         [&entries](const QString &, const QString &t,
                                    const QString &a, const QString &) {
            entries.insert(t + QLatin1Char('\n') + a);
        });
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&logs](const QString &m) { logs << m; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("hoarder")});
        QThread::msleep(300);
        pump(300);
    }
    QDir(dir).removeRecursively();
    EXPECT_EQ(entries.size(), LuaEngine::kMaxPaletteEntries)
        << entries.size() << " distinct entries reached the GUI";
    const QString refusedAt =
        QStringLiteral("refused at %1: ").arg(LuaEngine::kMaxPaletteEntries + 1);
    EXPECT_TRUE(std::any_of(logs.cbegin(), logs.cend(), [&](const QString &m) {
        return m.startsWith(refusedAt) && m.contains(QStringLiteral("ants.palette.register"));
    })) << qPrintable(logs.join(QLatin1Char('|')));
    EXPECT_TRUE(logs.contains(QStringLiteral("replace true")))
        << "an entry already held must still be replaceable at the cap: "
        << qPrintable(logs.join(QLatin1Char('|')));
}

// ANTS-6507 — ants.send refuses one message over kMaxSendBytes, and past
// kMaxSendBytesPerSecond drops sends and reports how many.
TEST(LuaThreading, Ants6507SendIsCappedPerMessageAndPerSecond) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("typist"),
              "ants.on('load', function()\n"
              "  local ok, err = pcall(ants.send, string.rep('x', 100 * 1024))\n"
              "  ants.log('oversize ' .. tostring(ok) .. ' ' .. tostring(err))\n"
              "  local chunk = string.rep('y', 10 * 1024)\n"
              "  for i = 1, 100 do ants.send(chunk) end\n"
              "end)\n");
    qint64 sentBytes = 0;
    bool oversizeSent = false;
    QStringList logs;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::sendToTerminal,
                         [&](const QString &t) {
            sentBytes += t.size();
            if (t.size() > 50 * 1024) oversizeSent = true;
        });
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&logs](const QString &m) { logs << m; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("typist")});
        pump(500);
        runHealthTick(pm);
    }
    QDir(dir).removeRecursively();
    EXPECT_FALSE(oversizeSent) << "a 100 KiB send reached the terminal";
    EXPECT_TRUE(std::any_of(logs.cbegin(), logs.cend(), [](const QString &m) {
        return m.startsWith(QStringLiteral("oversize false")) && m.contains(QStringLiteral("ants.send"));
    })) << qPrintable(logs.join(QLatin1Char('|')));
    EXPECT_LE(sentBytes, 2 * LuaEngine::kMaxSendBytesPerSecond)
        << sentBytes << " bytes of 1 MiB sent reached the terminal";
    EXPECT_GT(sentBytes, 0);
    EXPECT_TRUE(std::any_of(logs.cbegin(), logs.cend(), [](const QString &m) {
        return m.contains(QStringLiteral("dropped")) && m.contains(QStringLiteral("ants.send"));
    })) << "the dropped sends were not reported: " << qPrintable(logs.join(QLatin1Char('|')));
}

namespace {

// Counts the queued calls delivered to the object it filters.
class MetaCallCounter : public QObject {
public:
    int calls = 0;

    bool eventFilter(QObject *, QEvent *e) override {
        if (e->type() == QEvent::MetaCall) ++calls;
        return false;
    }
};

}  // namespace

TEST(LuaThreading, Ants5107BroadcastSkipsPluginsWithoutAHandler) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("quiet"), "ants.on('load', function() end)\n");
    addPlugin(dir, QStringLiteral("listener"),
              "ants.on('theme_changed', function(t) ants.log('theme:' .. t) end)\n");
    int quietCalls = -1;
    bool heardEarly = false;
    {
        PluginManager pm;
        QObject::connect(&pm, &PluginManager::logMessage,
                         [&heardEarly](const QString &m) {
            if (m == QStringLiteral("theme:early")) heardEarly = true;
        });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("quiet"), QStringLiteral("listener")});
        // Fired before either init.lua has run, so it must still arrive.
        pm.fireEvent(PluginEvent::ThemeChanged, QStringLiteral("early"));
        pump(300);
        LuaEngine *quiet = pm.engineFor(QStringLiteral("quiet"));
        ASSERT_NE(quiet, nullptr);
        auto *counter = new MetaCallCounter;
        counter->moveToThread(quiet->thread());
        QMetaObject::invokeMethod(quiet, [quiet, counter]() {
            quiet->installEventFilter(counter);
        }, Qt::BlockingQueuedConnection);
        for (int i = 0; i < 100; ++i)
            pm.fireEvent(PluginEvent::ThemeChanged, QStringLiteral("x"));
        // This call is itself one queued call, so a clean result is 1.
        QMetaObject::invokeMethod(quiet, [quiet, counter, &quietCalls]() {
            quiet->removeEventFilter(counter);
            quietCalls = counter->calls;
            delete counter;
        }, Qt::BlockingQueuedConnection);
    }
    QDir(dir).removeRecursively();
    EXPECT_EQ(quietCalls, 1) << "a plugin with no theme_changed handler was "
                                "sent " << quietCalls - 1 << " of them";
    EXPECT_TRUE(heardEarly) << "an event fired while the plugin loaded was lost";
}

// S8 (ANTS-6535) — ~MainWindow deletes the plugin manager in its own body.
// Left to die as a child widget, its unload ran after MainWindow's members
// were gone, and a plugin's unload settings.set wrote into a destroyed
// Config (R3 shows that call is delivered). Source scrape: a MainWindow
// cannot be built headless in this bundle.
TEST(LuaThreading, Ants6535MainWindowDeletesPluginManagerFirst) {
    const std::string body = ants_test::squashWhitespace(
        ants_test::slurpFunctionBody(ants_test::slurpMainWindow(),
                                     "MainWindow::~MainWindow()"));
    ASSERT_FALSE(body.empty()) << "~MainWindow not found";
    EXPECT_NE(body.find("delete m_pluginManager;"), std::string::npos)
        << "~MainWindow must delete m_pluginManager while m_config lives";
}

// ANTS-6846 — a permission prompt runs a nested event loop inside
// scanAndLoad(), which delivers an earlier plugin's palette entry before the
// load ends, so before pluginsReloaded. A clear on pluginsReloaded, as
// MainWindow had, loses it; pluginsAboutToLoad fires before any of it.
TEST(LuaThreading, Ants6846EntryDeliveredDuringAPromptSurvivesTheLoad) {
    const QString dir = makeTeardownDir();
    addPlugin(dir, QStringLiteral("a_menu"),
              "ants.on('load', function()\n"
              "  ants.palette.register({title = 'Early', action = 'a'})\n"
              "end)\n");
    addPlugin(dir, QStringLiteral("b_asker"), "", /*settingsPermission=*/true);
    QStringList order;
    bool prompted = false;
    {
        PluginManager pm;
        pm.setGrantStore([](const QString &) { return QStringList{}; },
                         [](const QString &, const QStringList &) {});
        pm.setPermissionPrompt([&prompted](const PluginInfo &,
                                           const QStringList &requested) {
            prompted = true;
            pump(500);   // QDialog::exec()'s nested loop
            return requested;
        });
        QObject::connect(&pm, &PluginManager::pluginsAboutToLoad,
                         [&order]() { order << QStringLiteral("about-to-load"); });
        QObject::connect(&pm, &PluginManager::pluginsReloaded,
                         [&order]() { order << QStringLiteral("reloaded"); });
        QObject::connect(&pm, &PluginManager::paletteEntryRegistered,
                         [&order](const QString &, const QString &title,
                                  const QString &, const QString &) { order << title; });
        pm.setPluginDir(dir + QStringLiteral("/plugins"));
        pm.scanAndLoad({QStringLiteral("a_menu"), QStringLiteral("b_asker")});
        pump(300);
    }
    QDir(dir).removeRecursively();
    ASSERT_TRUE(prompted) << "b_asker's load did not prompt";
    EXPECT_EQ(order, (QStringList{QStringLiteral("about-to-load"),
                                  QStringLiteral("Early"),
                                  QStringLiteral("reloaded")}))
        << qPrintable(order.join(QStringLiteral(", ")));
}

// ANTS-6846 — MainWindow::setupPlugins connects every PluginManager signal
// before scanAndLoad(), and clears its palette entries on pluginsAboutToLoad.
// Source scrape: a MainWindow cannot be built headless in this bundle.
TEST(LuaThreading, Ants6846SetupPluginsConnectsBeforeItLoads) {
    const std::string body = ants_test::slurpFunctionBody(
        ants_test::slurpMainWindow(), "void MainWindow::setupPlugins()");
    ASSERT_FALSE(body.empty()) << "setupPlugins not found";
    const size_t load = body.find("m_pluginManager->scanAndLoad(");
    const size_t lastConnect = body.rfind("connect(m_pluginManager,");
    ASSERT_NE(load, std::string::npos);
    ASSERT_NE(lastConnect, std::string::npos);
    EXPECT_GT(load, lastConnect) << "a connect runs after scanAndLoad()";
    EXPECT_NE(body.find("&PluginManager::pluginsAboutToLoad"), std::string::npos)
        << "the palette clear is not on pluginsAboutToLoad";
}

// ANTS-6645 — INV-9: the worker stack holds twice the deepest recursion the
// sandbox allows, string.gsub callbacks nested to Lua's C-call limit. Each
// level records how far down the stack it reached; the frame address, not a
// local's, so a sanitizer's fake stack cannot hide the real depth.
namespace {
uintptr_t g_lowestFrame = 0;

int recordFrame(lua_State *) {
    const auto at = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (g_lowestFrame == 0 || at < g_lowestFrame) g_lowestFrame = at;
    return 0;
}
}  // namespace

TEST(LuaThreading, Ants6645WorkerStackHoldsTwiceTheDeepestRecursion) {
    size_t used = 0;
    QString err;
    QThread *worker = QThread::create([&used, &err] {
        const auto top = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        g_lowestFrame = 0;
        lua_State *L = luaL_newstate();
        luaL_openlibs(L);
        lua_register(L, "record_frame", recordFrame);
        const int r = luaL_dostring(L,
            "local function g()\n"
            "  return (string.gsub('a', 'a', function()\n"
            "    record_frame() return g() end))\n"
            "end\n"
            "local ok, e = pcall(g)\n"
            "return tostring(e)\n");
        err = QString::fromUtf8(lua_tostring(L, -1));
        if (r != LUA_OK) err.prepend(QStringLiteral("script failed: "));
        lua_close(L);
        used = top - g_lowestFrame;
    });
    worker->setStackSize(LuaEngine::kWorkerStackBytes);
    worker->start();
    worker->wait();
    delete worker;
    ASSERT_TRUE(err.contains(QStringLiteral("stack overflow")))
        << "the recursion did not reach Lua's C-call limit: " << qPrintable(err);
    EXPECT_LE(used * 2, static_cast<size_t>(LuaEngine::kWorkerStackBytes))
        << "recursion to the C-call limit used " << used / 1024
        << " KiB of a " << LuaEngine::kWorkerStackBytes / 1024 << " KiB stack";
}
