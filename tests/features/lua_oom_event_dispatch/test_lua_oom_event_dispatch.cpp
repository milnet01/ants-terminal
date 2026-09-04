// Feature-conformance test for spec.md — ANTS-4442.
//
//   INV-1 — dispatch at the heap budget does not abort.
//   INV-2 — the failure is reported.
//   INV-3 — the engine is still usable afterwards.
//   INV-4 — ordinary dispatch is unchanged.
//   INV-5 — the pushes happen inside a protected call.
//
// Links against src/luaengine.cpp + Lua 5.4. Runs without Qt GUI.
//
// PRE-FIX BEHAVIOUR IS abort(), NOT A FAILED ASSERTION — measured, by
// reverting the fix and running this: SIGABRT out of lua_pushstring called
// from fireEvent, which ctest reports as "Subprocess aborted".
//
// That is still a clean red. ctest launches each registered test as its own
// process, so the abort takes this test and no sibling with it. INV-5 is
// kept anyway: it fails as an ordinary assertion, which is a far more
// legible signal than a core dump for anyone who reverts the structure
// without reaching the memory budget.

#include "luaengine.h"

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <string>

#ifndef SRC_LUAENGINE_CPP_PATH
#error "SRC_LUAENGINE_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// MAX_LUA_MEMORY is 10 MiB. Hold 8 MiB in a global so it cannot be
// collected, leaving under 2 MiB of headroom; the dispatch below then asks
// for 3 MiB, which the capped allocator must refuse. Both numbers are
// deliberately far from the cap so ordinary interpreter overhead cannot
// move the test either way.
const char *kScript =
    "_G.hog = {}\n"
    "for i = 1, 8 do\n"
    "  _G.hog[i] = string.rep('x', 1024 * 1024)\n"
    "end\n"
    "ants.on('line', function(d) ants.log('ran:' .. #d) return true end)\n";

bool logHas(const QStringList &log, const char *needle) {
    for (const QString &line : log)
        if (line.contains(QLatin1String(needle))) return true;
    return false;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(LuaOomEventDispatch, Ants4442DispatchAtBudgetDoesNotAbort) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/hog.lua");
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
        << "the 8 MiB hog must fit under the 10 MiB budget; if this fails "
           "the test is not measuring what it claims";

    // INV-4 — ordinary dispatch first, while there is headroom. A small
    // payload reaches the handler with its byte count intact.
    log.clear();
    engine.fireEvent(PluginEvent::Line, QStringLiteral("abc"));
    expect(logHas(log, "ran:3"), "INV-4/ordinary-dispatch-unchanged",
           QStringLiteral("expected the handler to receive 3 bytes, log was "
                          "%1").arg(log.join(QStringLiteral(","))));

    // INV-1 — 3 MiB against under 2 MiB of headroom. The allocator refuses,
    // Lua raises a memory error, and pre-fix that error is raised outside
    // any protected call. Returning from this line at all is the assertion.
    log.clear();
    engine.fireEvent(PluginEvent::Line,
                     QString(qsizetype{3} * 1024 * 1024, QLatin1Char('y')));

    expect(true, "INV-1/no-abort-at-budget",
           QStringLiteral("unreachable pre-fix — the process aborts"));

    // INV-2 — reported rather than swallowed.
    expect(logHas(log, "Plugin error"), "INV-2/failure-is-reported",
           QStringLiteral("expected a plugin-error message, log was %1")
               .arg(log.join(QStringLiteral(","))));

    // INV-3 — and the stack was left balanced, so the engine still works.
    log.clear();
    engine.fireEvent(PluginEvent::Line, QStringLiteral("ok"));
    expect(logHas(log, "ran:2"), "INV-3/engine-usable-after-failure",
           QStringLiteral("expected the handler to run again after the "
                          "refused dispatch, log was %1")
               .arg(log.join(QStringLiteral(","))));

    engine.shutdown();
    EXPECT_EQ(0, expect_finish());
}

// INV-5 — the structure, so a regression is visible as a failure and not
// only as a bundle that stopped existing.
TEST(LuaOomEventDispatch, Inv5PushesAreProtected) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_LUAENGINE_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-5: luaengine.cpp not readable";

    const auto fireAt = cpp.find("bool LuaEngine::fireEvent(");
    ASSERT_NE(fireAt, std::string::npos) << "INV-5: fireEvent not found";
    const std::string body = cpp.substr(fireAt);

    expect(contains(body, "lua_checkstack"),
           "INV-5: stack space is secured by a call that reports failure "
           "rather than raising");
    expect(contains(body, "luaPushHandlerAndArg"),
           "INV-5: the handler and argument are pushed through the "
           "protected helper");
    expect(!contains(body.substr(0, body.find("lua_pcall")),
                     "lua_pushstring"),
           "INV-5: nothing allocates a Lua string before the first "
           "protected call");
    EXPECT_EQ(0, expect_failures());
}
