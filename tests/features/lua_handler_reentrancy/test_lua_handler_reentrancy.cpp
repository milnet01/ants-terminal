// Feature-conformance test for spec.md — ANTS-4441.
//
//   INV-1 — a handler registered during a dispatch does not run in it.
//   INV-2 — it runs on the next dispatch.
//   INV-3 — both dispatches complete (under ASan this is the UAF catch).
//
// Links against src/luaengine.cpp + Lua 5.4. Runs without Qt GUI.
// Pre-fix code range-iterates the live std::vector inside m_handlers
// while lua_pcall runs a handler that reallocates it.

#include "luaengine.h"

#include "../../_support/expect.h"

#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

// Three handlers on `line`. The first registers twenty more on the same
// event (vector reallocation) and one on a different event (QHash insert
// — relocates every value, including the vector being iterated).
const char *kScript =
    "ants.on('line', function(d)\n"
    "  ants.log('early:1')\n"
    "  for i = 1, 20 do\n"
    "    ants.on('line', function(e) ants.log('late') end)\n"
    "  end\n"
    "  ants.on('theme_changed', function(e) end)\n"
    "  return true\n"
    "end)\n"
    "ants.on('line', function(d) ants.log('early:2') return true end)\n"
    "ants.on('line', function(d) ants.log('early:3') return true end)\n";

int countOf(const QStringList &log, const char *needle) {
    int n = 0;
    for (const QString &line : log)
        if (line == QLatin1String(needle)) ++n;
    return n;
}

} // namespace

TEST(LuaHandlerReentrancy, Ants4441RegisterFromHandlerIsSafe) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/reentrant.lua");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(kScript);
    }

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize()) << "LuaEngine::initialize failed";

    QStringList log;
    QObject::connect(&engine, &LuaEngine::logMessage,
                     [&log](const QString &m) { log.append(m); });

    ASSERT_TRUE(engine.loadScript(path))
        << "the registration script must load; a plugin registering a "
           "handler from a handler is an ordinary idiom";
    log.clear();

    // INV-1 + INV-3 — the dispatch completes, and it runs the three
    // handlers that existed when it started. Pre-fix this loop walks a
    // freed buffer once the twenty push_backs reallocate it.
    engine.fireEvent(PluginEvent::Line, QStringLiteral("x"));

    expect(countOf(log, "early:1") == 1 && countOf(log, "early:2") == 1 &&
               countOf(log, "early:3") == 1,
           "INV-1a/original-handlers-all-ran",
           QStringLiteral("expected each of early:1..3 exactly once, got %1")
               .arg(log.join(QStringLiteral(","))));
    expect(countOf(log, "late") == 0,
           "INV-1b/late-handlers-did-not-run",
           QStringLiteral("a handler registered during the dispatch must "
                          "not run in that dispatch; %1 did")
               .arg(countOf(log, "late")));

    // INV-2 — the twenty are live from the next dispatch on. The first
    // handler runs again and registers twenty more, so this dispatch
    // sees exactly the 23 handlers snapshotted at its start.
    log.clear();
    engine.fireEvent(PluginEvent::Line, QStringLiteral("y"));
    expect(countOf(log, "late") == 20,
           "INV-2/late-handlers-run-next-time",
           QStringLiteral("expected 20 late handlers on the second "
                          "dispatch, got %1").arg(countOf(log, "late")));

    engine.shutdown();
}
