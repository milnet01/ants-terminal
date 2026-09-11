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

// ---------------------------------------------------------------------
// ANTS-5070 — INV-6: a raise while lua_ants_warn's accumulating message is
// alive longjmps past it (a real QString leak, LeakSanitizer-only). Guards
// ants.get_output / ants.settings.get in the same fire since ANTS-5070
// found the identical pattern in both (a live QString pushed via
// pushQString).
//
// ANTS-5070 names the trigger as an argument whose __tostring metamethod
// errors. sandboxEnvironment()'s allowlist has neither setmetatable nor
// getmetatable, so a sandboxed script cannot attach a __tostring at all.
// Driving luaL_tolstring's default formatting past the VM's budget does not
// reach it either: under pressure the emergency collection reclaims each
// coercion's discarded string before the next one needs the room, so the
// raise inside warn's loop cannot be forced (measured). This case is
// therefore a GUARD: warn under memory pressure returns to its caller, and
// get_output / settings.get still answer afterwards. warn's change is
// verified by review.
// ---------------------------------------------------------------------
TEST(LuaOomEventDispatch, Ants5070WarnUnderPressureNoLeak) {
    expect_reset();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/warn.lua");

    // Numeric arguments: luaL_tolstring allocates a string for each, so the
    // coercions fail once the fill below has left too little. A call takes
    // at most about 250 arguments (Lua's register limit — measured: 2000
    // failed to compile, so the script never loaded), hence 200.
    QString warnArgs;
    for (int i = 0; i < 200; ++i) {
        if (i) warnArgs += QLatin1Char(',');
        warnArgs += QString::number(1000000 + i);
    }

    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        // The fill runs inside the handler, so loading the script and
        // registering the handler need no memory the fill has taken. Large
        // chunks first, then small ones, leave only a sliver for warn. The
        // fill is released before anything is logged.
        const QString script =
            QStringLiteral(
                "ants.on('line', function(d)\n"
                "  local hog, i = {}, 1\n"
                "  local function grow() hog[i] = string.rep('x', 8192) i = i + 1 end\n"
                "  while pcall(grow) do end\n"
                "  local function top() hog[i] = string.rep('y', 48) i = i + 1 end\n"
                "  while pcall(top) do end\n"
                "  local lastOk, lastErr\n"
                "  for j = 1, 5 do\n"
                "    lastOk, lastErr = pcall(warn, %1)\n"
                "  end\n"
                "  hog = nil  -- the next failed allocation collects it\n"
                "  ants.log('warnresult:' .. tostring(lastOk) .. ':' .. tostring(lastErr))\n"
                "  local out = ants.get_output(10)\n"
                "  local val = ants.settings.get('mykey')\n"
                "  ants.log('guard:' .. tostring(out) .. '|' .. tostring(val))\n"
                "  return true\n"
                "end)\n").arg(warnArgs);
        f.write(script.toUtf8());
    }

    LuaEngine engine;
    engine.setPermissions(QStringList{QStringLiteral("settings")});
    ASSERT_TRUE(engine.initialize()) << "LuaEngine::initialize failed";

    QStringList log;
    QObject::connect(&engine, &LuaEngine::logMessage,
                     [&log](const QString &m) { log.append(m); });
    QObject::connect(&engine, &LuaEngine::settingsGetRequested,
                     [](const QString &, const QString &, QString &out) {
                         out = QStringLiteral("canned-value");
                     });

    engine.setRecentOutput(QStringLiteral("line1\nline2\nline3"));
    ASSERT_TRUE(engine.loadScript(path))
        << "the fill + handler script must load; if this fails the test "
           "is not measuring what it claims";

    engine.fireEvent(PluginEvent::Line, QStringLiteral("trigger"));

    // Reaching here at all matters (no abort), same shape as INV-1 above.
    // The leak itself is invisible outside ASan — build-asan's
    // LeakSanitizer is the real signal; these confirm the ordinary-error
    // path actually ran rather than the setup silently not applying
    // enough pressure.
    // GUARD, not a red case. warn's raise-with-a-live-message path cannot be
    // reached deterministically: the sandbox strips setmetatable, so no
    // __tostring can fail, and under pressure the emergency collection
    // reclaims each coercion's discarded string before the next needs the
    // room (measured: all five calls succeeded under this fill). What this
    // pins is that warn under pressure returns to its caller.
    expect(logHas(log, "warnresult:"),
           "ANTS-5070/warn-under-pressure-returns",
           QStringLiteral("expected the handler to get past its warn calls, "
                          "log was %1")
               .arg(log.join(QStringLiteral(","))));

    expect(logHas(log, "guard:line1") && logHas(log, "canned-value"),
           "ANTS-5070/get-output-and-settings-get-guard",
           QStringLiteral("expected get_output/settings.get to still return "
                          "their configured values after the failed warn "
                          "calls, log was %1")
               .arg(log.join(QStringLiteral(","))));

    engine.shutdown();
    EXPECT_EQ(0, expect_finish());
}
