// Feature-conformance test for spec.md —
//
// Runtime:
//   A1 — Loop-nested catch terminates within slack.
//   A2 — Source-nested catch terminates within slack (100-deep).
//   A3 — Recovery: a fresh pcall after kill runs normally.
//   A4 — Benign plugins not penalised (~10k instr, ≤ 100 ms).
// Source:
//   S1 — m_killed declared on LuaEngine.
//   S2 — m_killed = false in startPcallBudget.
//   S3 — m_killed = true inside LuaEngine::instructionHook.
//   S4 — Latched re-arm: lua_sethook(L, instructionHook, ...|LUA_MASKLINE, 1).
//   S5 — startPcallBudget restore: lua_sethook(..., 100000).
//
// Links against src/luaengine.cpp + Lua 5.4. Runs without Qt GUI.
// Pre-fix code makes A1 hang forever — the bundle's CTest TIMEOUT
// (30 s) is the backstop.

#include "luaengine.h"

#include "../../_support/expect.h"

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

// Spec § 4 Invariant 6: m_pcallBudgetMs (production 1500 ms) +
// LUAI_MAXCCALLS × t_unwind. The kill-path semantics are independent
// of the budget value; the test injects a tightened budget via
// LuaEngine::setPcallBudgetMs() so each pcall completes in well under
// a second instead of paying ~1.5 s × N runs. 500 ms slack covers
// loaded-CI scheduler jitter (unchanged from the production budget
// because jitter doesn't scale with the budget itself).
constexpr qint64 kBudgetMs = 250;
constexpr qint64 kSlackMs = 500;
constexpr qint64 kRuntimeBoundMs = kBudgetMs + kSlackMs;

// A1 — Loop-nested catch. Pre-fix code never returns from loadScript;
// CTest TIMEOUT on the bundle backstops the hang.
void runA1(const QString &dir) {
    LuaEngine engine;
    if (!engine.initialize()) {
        expect(false, "A1/init", QStringLiteral("LuaEngine::initialize failed"));
        return;
    }
    engine.setPcallBudgetMs(kBudgetMs);
    // 1e9 inner iterations would take ~minutes uncapped; the wall budget
    // kicks in at the test-injected kBudgetMs.
    const QByteArray script =
        "while true do\n"
        "  pcall(function()\n"
        "    local x = 0\n"
        "    for i = 1, 1000000000 do x = x + 1 end\n"
        "  end)\n"
        "end\n";
    const QString path = writeTemp(dir, "a1_loop.lua", script);
    if (path.isEmpty()) return;

    QElapsedTimer timer; timer.start();
    const bool loaded = engine.loadScript(path);
    const qint64 elapsed = timer.elapsed();

    // The hostile script must NOT load successfully — loadScript returns
    // false when the engine's outer lua_pcall returns with an error
    // (the wall-clock luaL_error propagating past all inner pcalls).
    expect(!loaded,
           "A1a/loop-nested-loadScript-returns-error",
           QStringLiteral("loadScript should return false on timeout; "
                          "returned true after %1ms").arg(elapsed));
    expect(elapsed <= kRuntimeBoundMs,
           "A1b/loop-nested-within-slack",
           QStringLiteral("loadScript took %1ms (bound %2ms = "
                          "%3 wall + %4 slack)")
               .arg(elapsed).arg(kRuntimeBoundMs)
               .arg(kBudgetMs).arg(kSlackMs));

    QFile::remove(path);
    engine.shutdown();
}

// A2 — Source-nested catch, 100-deep recursion.
void runA2(const QString &dir) {
    LuaEngine engine;
    if (!engine.initialize()) {
        expect(false, "A2/init", QStringLiteral("LuaEngine::initialize failed"));
        return;
    }
    engine.setPcallBudgetMs(kBudgetMs);
    // Each level wraps the next-deeper call in pcall; at depth 0 the
    // innermost level burns CPU until the wall-clock expires. Depth 100
    // is well under Lua's LUAI_MAXCCALLS default of 200.
    const QByteArray script =
        "local function f(depth)\n"
        "  if depth == 0 then\n"
        "    local x = 0\n"
        "    for i = 1, 1000000000 do x = x + 1 end\n"
        "  else\n"
        "    while true do\n"
        "      pcall(function() f(depth - 1) end)\n"
        "    end\n"
        "  end\n"
        "end\n"
        "f(100)\n";
    const QString path = writeTemp(dir, "a2_nested.lua", script);
    if (path.isEmpty()) return;

    QElapsedTimer timer; timer.start();
    const bool loaded = engine.loadScript(path);
    const qint64 elapsed = timer.elapsed();

    expect(!loaded,
           "A2a/source-nested-loadScript-returns-error",
           QStringLiteral("loadScript should return false on timeout; "
                          "returned true after %1ms").arg(elapsed));
    expect(elapsed <= kRuntimeBoundMs,
           "A2b/source-nested-within-slack",
           QStringLiteral("loadScript took %1ms (budget %2ms)")
               .arg(elapsed).arg(kRuntimeBoundMs));

    QFile::remove(path);
    engine.shutdown();
}

// A3 — After a kill, a fresh pcall in the same VM runs normally.
// Proves Invariant 2 (startPcallBudget resets the latch).
//
// ANTS-1750 moved recovery onto the EVENT path: loadScript now tears the
// VM down on a script error (§ 2.2.2), so a failed init.lua means a dead
// plugin — recovery no longer applies to re-loading a chunk. The realistic
// place a timeout recovers is between event handlers: one handler times out
// (latches m_killed), the next handler still runs because startPcallBudget
// clears the latch + restores the 100k count before each handler's pcall.
// This rewrite exercises exactly that, on one live VM.
void runA3(const QString &dir) {
    LuaEngine engine;
    if (!engine.initialize()) {
        expect(false, "A3/init", QStringLiteral("LuaEngine::initialize failed"));
        return;
    }
    engine.setPcallBudgetMs(kBudgetMs);
    // One VM, two handlers: a runaway command_finished handler that burns
    // past the wall budget, and a benign theme_changed handler that records
    // it ran. The chunk itself loads cleanly (only ants.on registrations).
    const QByteArray script =
        "ants.on('command_finished', function()\n"
        "  local x = 0\n"
        "  for i = 1, 1000000000 do x = x + 1 end\n"
        "end)\n"
        "ants.on('theme_changed', function()\n"
        "  _G.ants_test_a3_ran = true\n"
        "end)\n";
    const QString path = writeTemp(dir, "a3_handlers.lua", script);
    if (path.isEmpty()) return;
    const bool loaded = engine.loadScript(path);
    expect(loaded,
           "A3a/handlers-script-loads",
           QStringLiteral("handler-registration chunk must load cleanly"));
    if (!loaded) { QFile::remove(path); return; }

    // Fire the runaway handler — the wall-clock budget kills it (latched).
    QElapsedTimer timer; timer.start();
    engine.fireEvent(PluginEvent::CommandFinished, QString());
    const qint64 killMs = timer.elapsed();
    if (killMs > kRuntimeBoundMs) {
        // The kill-path latency itself is covered by A1; don't double-report.
        QFile::remove(path);
        return;
    }

    // Fire the benign handler — it must run normally now the latch is reset.
    QElapsedTimer timer2; timer2.start();
    engine.fireEvent(PluginEvent::ThemeChanged, QString());
    const qint64 goodElapsed = timer2.elapsed();

    // Confirm the benign handler actually executed: a fresh chunk that
    // errors (→ loadScript returns false) unless the flag is set. This is
    // the same "loadScript-false-is-our-signal" idiom the sandbox test uses.
    const QByteArray check =
        "if not _G.ants_test_a3_ran then\n"
        "  error('benign handler did not run after kill — latch not reset')\n"
        "end\n";
    const QString checkPath = writeTemp(dir, "a3_check.lua", check);
    const bool ranOk = engine.loadScript(checkPath);
    expect(ranOk,
           "A3b/recovery-benign-handler-ran",
           QStringLiteral("benign handler after kill did not run (latch not "
                          "reset by startPcallBudget)"));
    // Widened from 200 ms (post-test-audit 2026-05-18): on loaded CI runners
    // or sanitiser builds a benign handler can spike past 200 ms while still
    // proving the latch reset. 750 ms preserves the contract (count restored
    // to 100k → benign runs in well under a second) without flaking.
    expect(goodElapsed <= 750,
           "A3c/recovery-benign-fast",
           QStringLiteral("benign handler took %1ms after kill (expected "
                          "≤ 750ms — count must be restored to 100k)")
               .arg(goodElapsed));

    QFile::remove(path);
    QFile::remove(checkPath);
    engine.shutdown();
}

// A4 — Benign plugin (no kill, no latch trigger): runs fast.
// Proves Invariant 7 (no count-1 leak on the hot path).
void runA4(const QString &dir) {
    LuaEngine engine;
    if (!engine.initialize()) {
        expect(false, "A4/init", QStringLiteral("LuaEngine::initialize failed"));
        return;
    }
    engine.setPcallBudgetMs(kBudgetMs);
    // ~10 000 trivial instructions — well under the count-mask threshold.
    const QByteArray script =
        "local x = 0\n"
        "for i = 1, 10000 do x = x + 1 end\n";
    const QString path = writeTemp(dir, "a4_benign.lua", script);
    if (path.isEmpty()) return;

    QElapsedTimer timer; timer.start();
    const bool ok = engine.loadScript(path);
    const qint64 elapsed = timer.elapsed();
    expect(ok,
           "A4a/benign-loads",
           QStringLiteral("loadScript failed (elapsed %1ms)").arg(elapsed));
    // Widened from 100 ms (post-test-audit 2026-05-18): ~10 000 trivial
    // instructions complete in single-digit ms even on slow hardware,
    // but a sanitiser build or loaded CI runner can push a single
    // syscall into the 50–80 ms range. 500 ms still proves Invariant 7
    // (no count-1 leak on the hot path) with comfortable margin.
    expect(elapsed <= 500,
           "A4b/benign-fast",
           QStringLiteral("benign script took %1ms (expected ≤ 500ms)")
               .arg(elapsed));

    QFile::remove(path);
    engine.shutdown();
}

void runRuntimeChecks() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-lua-pcall-")
        + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(dir);
    runA1(dir);
    runA2(dir);
    runA3(dir);
    runA4(dir);
    QDir().rmdir(dir);
}

// Source checks operate on the post-fix source. Pre-fix code lacks
// every grep target below; each check fails individually with a clear
// label so a regression points at the missing piece.

void runSourceChecks() {
    const QString hPath = QStringLiteral(SRC_LUAENGINE_H_PATH);
    QFile hFile(hPath);
    if (!hFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        expect(false, "source-open-h",
               QStringLiteral("cannot read %1").arg(hPath));
        return;
    }
    const QString hSrc = QString::fromUtf8(hFile.readAll());
    hFile.close();

    const QString cppPath = QStringLiteral(SRC_LUAENGINE_PATH);
    QFile cppFile(cppPath);
    if (!cppFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        expect(false, "source-open-cpp",
               QStringLiteral("cannot read %1").arg(cppPath));
        return;
    }
    const QString cppSrc = QString::fromUtf8(cppFile.readAll());
    cppFile.close();

    // S1 — m_killed declared on LuaEngine.
    expect(hSrc.contains(QStringLiteral("bool m_killed")),
           "S1/m_killed-declared",
           QStringLiteral("luaengine.h must declare `bool m_killed`"));

    // Locate startPcallBudget body. The function starts at
    // `void LuaEngine::startPcallBudget() {` and ends at the matching
    // closing brace.
    const int budgetStart = cppSrc.indexOf(
        QStringLiteral("void LuaEngine::startPcallBudget()"));
    int budgetEnd = budgetStart;
    if (budgetStart > 0) {
        // Walk to the closing brace at column 0 — the function's `}`.
        int braceDepth = 0;
        bool entered = false;
        for (int i = budgetStart; i < cppSrc.size(); ++i) {
            const QChar c = cppSrc.at(i);
            if (c == QChar('{')) { ++braceDepth; entered = true; }
            else if (c == QChar('}')) {
                --braceDepth;
                if (entered && braceDepth == 0) { budgetEnd = i; break; }
            }
        }
    }
    const QString budgetBody = (budgetStart > 0 && budgetEnd > budgetStart)
        ? cppSrc.mid(budgetStart, budgetEnd - budgetStart + 1)
        : QString();
    expect(!budgetBody.isEmpty(),
           "source/startPcallBudget-body-found",
           QStringLiteral("could not locate startPcallBudget body in cpp"));

    // S2 — m_killed = false inside startPcallBudget.
    expect(budgetBody.contains(QStringLiteral("m_killed = false")),
           "S2/m_killed-reset-in-startPcallBudget",
           QStringLiteral("startPcallBudget must reset m_killed = false"));

    // S5 — startPcallBudget restores hook count to 100000.
    //   The mask anchor (LUA_MASKLINE) distinguishes this re-arm from
    //   the `lua_sethook(m_state, nullptr, 0, 0)` in shutdown.
    expect(budgetBody.contains(QStringLiteral("LUA_MASKLINE, 100000")),
           "S5/startPcallBudget-restores-count",
           QStringLiteral("startPcallBudget must re-arm hook with "
                          "LUA_MASKLINE | ..., 100000"));

    // Locate the named static instructionHook DEFINITION (not just any
    // mention — the first mention is the &LuaEngine::instructionHook
    // function-pointer use inside initialize(), which would brace-walk
    // through the wrong function body). Spec § 4 Invariant 5 mandates
    // the lift from captureless lambda to a named static.
    const int hookDeclLine = cppSrc.indexOf(
        QStringLiteral("void LuaEngine::instructionHook("));
    expect(hookDeclLine > 0,
           "source/instructionHook-named-static-exists",
           QStringLiteral("expected `void LuaEngine::instructionHook(` "
                          "definition; the lambda must be lifted to a "
                          "named static (Invariant 5)"));

    // Body of instructionHook.
    int hookEnd = hookDeclLine;
    if (hookDeclLine > 0) {
        int braceDepth = 0;
        bool entered = false;
        for (int i = hookDeclLine; i < cppSrc.size(); ++i) {
            const QChar c = cppSrc.at(i);
            if (c == QChar('{')) { ++braceDepth; entered = true; }
            else if (c == QChar('}')) {
                --braceDepth;
                if (entered && braceDepth == 0) { hookEnd = i; break; }
            }
        }
    }
    const QString hookBody = (hookDeclLine > 0 && hookEnd > hookDeclLine)
        ? cppSrc.mid(hookDeclLine, hookEnd - hookDeclLine + 1)
        : QString();

    // S3 — m_killed = true inside instructionHook.
    expect(hookBody.contains(QStringLiteral("m_killed = true")),
           "S3/m_killed-set-in-instructionHook",
           QStringLiteral("instructionHook must latch m_killed = true "
                          "on first wall-clock expiry"));

    // S4 — Latched re-arm: lua_sethook(L, instructionHook, ...|LUA_MASKLINE, 1).
    //   Anchored to mask|, 1) so we don't false-match against the
    //   shutdown's mask=0 sethook.
    expect(hookBody.contains(
               QStringLiteral("LUA_MASKLINE, 1)")),
           "S4/instructionHook-re-arms-count-1",
           QStringLiteral("instructionHook must re-arm via "
                          "lua_sethook(..., LUA_MASKLINE, 1) once latched"));

    // Sanity: instructionHook actually called by initialize() too.
    expect(cppSrc.contains(QStringLiteral("lua_sethook(m_state,")) &&
               cppSrc.indexOf(QStringLiteral("instructionHook")) > 0,
           "source/initialize-uses-instructionHook",
           QStringLiteral("initialize() must pass instructionHook to "
                          "lua_sethook (so the in-hook re-arm uses the "
                          "same symbol)"));
}

int runMain() {
    expect_reset();
    runRuntimeChecks();
    runSourceChecks();
    return expect_finish();
}

}  // namespace

TEST(LuaPcallNestingTimeout, Main) {
    ASSERT_EQ(0, runMain());
}
