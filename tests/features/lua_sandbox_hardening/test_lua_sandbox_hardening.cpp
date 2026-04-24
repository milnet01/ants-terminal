// Feature-conformance test for spec.md —
//
// Invariant 1 — string.dump is nil after initialize().
// Invariant 2 — loadScript loads a valid text file.
// Invariant 3 — loadScript rejects a file starting with 0x1b.
// Invariant 4 — source: luaL_loadfilex("t") replaces luaL_dofile.
// Invariant 5 — source: shutdown() clears lua_sethook before lua_close.
// Invariant 6 — source: string.dump removal is scoped to the string
//               table, not a blanket global nil.
//
// Links against src/luaengine.cpp + Lua 5.4. Runs without Qt GUI.
// Exit 0 = all invariants hold. Non-zero = regression.

#include "luaengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

#include <lua5.4/lua.hpp>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const QString &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label,
                     qUtf8Printable(detail));
        ++g_failures;
    }
}

QString writeTemp(const QString &dir, const QString &basename,
                  const QByteArray &bytes) {
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/") + basename;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "[FAIL] setup: cannot write %s\n",
                     qUtf8Printable(path));
        ++g_failures;
        return {};
    }
    f.write(bytes);
    f.close();
    return path;
}

void runRuntimeChecks() {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation)
        + QStringLiteral("/ants-lua-sandbox-")
        + QUuid::createUuid().toString(QUuid::Id128);

    LuaEngine engine;
    if (!engine.initialize()) {
        std::fprintf(stderr, "[FAIL] setup: LuaEngine::initialize failed\n");
        ++g_failures;
        return;
    }

    // Invariant 1 — string.dump is nil after sandbox.
    //
    // The script runs under our sandbox so it can't access `type` on the
    // global registry directly to read its own state — we use _G writes
    // that the test can then retrieve via the C API. The script must
    // succeed even if string.dump is nil (checked via pcall).
    const QByteArray script =
        "_G.ants_test_string_dump_type = type(string.dump)\n"
        "local ok = pcall(function()\n"
        "  local f = function() end\n"
        "  return string.dump(f)\n"
        "end)\n"
        "_G.ants_test_dump_errored = not ok\n";
    const QString scriptPath = writeTemp(dir, "sandbox_check.lua", script);
    if (scriptPath.isEmpty()) return;

    const bool loaded = engine.loadScript(scriptPath);
    expect(loaded, "I2/valid-text-script-loads",
           QStringLiteral("plain-text .lua must still load via luaL_loadfilex"));

    // Reach into the engine's Lua state via friend-less extraction is
    // not available — but loadScript ran the script, which wrote its
    // observations to _G. We re-load a small script that throws on
    // unexpected state, using the fact that loadScript returns false
    // on pcall error as our signal.
    const QByteArray assertDumpNil =
        "if _G.ants_test_string_dump_type ~= 'nil' then\n"
        "  error('string.dump leaked: type=' .. tostring(_G.ants_test_string_dump_type))\n"
        "end\n"
        "if not _G.ants_test_dump_errored then\n"
        "  error('string.dump call did not error')\n"
        "end\n";
    const QString assertPath = writeTemp(dir, "sandbox_assert.lua", assertDumpNil);
    const bool assertLoaded = engine.loadScript(assertPath);
    expect(assertLoaded,
           "I1/string-dump-is-nil-and-call-errors",
           QStringLiteral("expected string.dump to be nil in the "
                          "sandbox and pcall-wrapped call to error; "
                          "loadScript returned false meaning the "
                          "assertion script threw"));

    // Invariant 3 — a file starting with 0x1b is rejected.
    // Lua's binary-chunk header is `\x1b Lua`. loadScript's peek catches
    // the first byte before we even reach luaL_loadfilex; confirm it's
    // still in place.
    QByteArray bytecodeHeader;
    bytecodeHeader.append('\x1b');
    bytecodeHeader.append("Lua");  // valid header signature
    bytecodeHeader.append('\x00');
    const QString bcPath = writeTemp(dir, "bytecode.lua", bytecodeHeader);
    const bool bcRejected = !engine.loadScript(bcPath);
    expect(bcRejected,
           "I3/bytecode-header-rejected",
           QStringLiteral("loadScript must return false when the first "
                          "byte is 0x1b (Lua binary-chunk header)"));

    engine.shutdown();

    // Cleanup.
    QFile::remove(scriptPath);
    QFile::remove(assertPath);
    QFile::remove(bcPath);
    QDir().rmdir(dir);
}

void runSourceChecks() {
    const QString path = QStringLiteral(SRC_LUAENGINE_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] source-open: cannot read %s\n",
                     qUtf8Printable(path));
        ++g_failures;
        return;
    }
    const QString src = QString::fromUtf8(f.readAll());
    f.close();

    // Invariant 4 — luaL_loadfilex with "t" mode.
    expect(src.contains(QStringLiteral("luaL_loadfilex")) &&
               (src.contains(QStringLiteral(", \"t\")")) ||
                src.contains(QStringLiteral(",\"t\")"))),
           "I4/luaL_loadfilex-text-only-mode",
           QStringLiteral("luaengine.cpp must call luaL_loadfilex(..., \"t\") "
                          "to reject binary chunks at the loader level"));
    expect(!src.contains(QStringLiteral("luaL_dofile(")),
           "I4b/raw-luaL_dofile-gone",
           QStringLiteral("luaL_dofile(...) forwards to loadfilex with "
                          "mode=nullptr (binary-accepting) — must be "
                          "replaced by the explicit luaL_loadfilex + "
                          "lua_pcall pair"));

    // Invariant 5 — hook cleared before lua_close in shutdown.
    //   The helper signature is `lua_sethook(L, func, mask, count)`.
    //   A nullptr func with mask=0 means "no hook".
    const int hookIdx = src.indexOf(
        QStringLiteral("lua_sethook(m_state, nullptr"));
    const int closeIdx = src.indexOf(QStringLiteral("lua_close(m_state)"));
    expect(hookIdx > 0 && closeIdx > 0 && hookIdx < closeIdx,
           "I5/hook-cleared-before-lua_close",
           QStringLiteral("expected lua_sethook(m_state, nullptr, ...) "
                          "BEFORE lua_close(m_state); hookIdx=%1 "
                          "closeIdx=%2").arg(hookIdx).arg(closeIdx));

    // Invariant 6 — string.dump removal scoped to the string table.
    expect(src.contains(QStringLiteral("lua_setfield(m_state, -2, \"dump\")")),
           "I6/string-dump-scoped-to-string-table",
           QStringLiteral("string.dump must be removed via "
                          "lua_setfield on the already-loaded string "
                          "table (not lua_setglobal, which would touch "
                          "the wrong slot)"));
    // Negative grep: make sure nobody accidentally did the wrong thing
    // by setting a global named "dump" — that wouldn't remove string.dump,
    // it would add a new global.
    expect(!src.contains(QStringLiteral("lua_setglobal(m_state, \"dump\")")),
           "I6b/no-misplaced-global-named-dump");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    runRuntimeChecks();
    runSourceChecks();

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
