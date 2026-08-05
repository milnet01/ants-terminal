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

#include "../../_support/expect.h"
#include "luaengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>
#include <QUuid>

// Unqualified — see the note in src/luaengine.cpp (ANTS-3727). The include path
// arrives via ants_lua_lib's PUBLIC LUA_INCLUDE_DIRS, which this bundle links.
#include <lua.hpp>

#include <cstdio>


#include <gtest/gtest.h>
ANTS_TEST_SCOPE();

namespace {



QString writeTemp(const QString &dir, const QString &basename,
                  const QByteArray &bytes) {
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/") + basename;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "[FAIL] setup: cannot write %s\n",
                     qUtf8Printable(path));
        expect(false, "setup-error", "");
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
        expect(false, "setup-error", "");
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

    // ANTS-1268 — allowlist behaviour. After sandboxEnvironment(), `_G`
    // must contain ONLY the documented-safe names; everything else
    // (including any global a future Lua release adds) must be nil. The
    // script enumerates _G and records any key outside the allowlist,
    // then the assert script throws if that set is non-empty or if a
    // representative denied global leaked / an allowed one vanished.
    const QByteArray allowlistProbe =
        "local allowed = {\n"
        "  _G=true,_VERSION=true,assert=true,error=true,ipairs=true,\n"
        "  next=true,pairs=true,pcall=true,print=true,select=true,\n"
        "  tonumber=true,tostring=true,type=true,warn=true,xpcall=true,\n"
        "  string=true,table=true,math=true,utf8=true,ants=true,\n"
        "}\n"
        "local extra = {}\n"
        "for k,_ in pairs(_G) do\n"
        // The engine is reused across the earlier check scripts, which
        // stashed `ants_test_*` scratch keys in _G; those are test
        // artefacts, not sandbox leaks — skip them.
        "  if type(k)=='string' and not allowed[k]\n"
        "     and string.sub(k,1,10) ~= 'ants_test_' then\n"
        "    extra[#extra+1] = k\n"
        "  end\n"
        "end\n"
        "_G.ants_test_extra_globals = table.concat(extra, ',')\n"
        "_G.ants_test_os_nil = (os == nil)\n"
        "_G.ants_test_setmeta_nil = (setmetatable == nil)\n"
        "_G.ants_test_collectgc_nil = (collectgarbage == nil)\n"
        "_G.ants_test_pairs_present = (pairs ~= nil)\n"
        "_G.ants_test_string_present = (string ~= nil)\n";
    const QString probePath = writeTemp(dir, "allowlist_probe.lua",
                                        allowlistProbe);
    expect(engine.loadScript(probePath),
           "I7-setup/allowlist-probe-loads", QString());
    const QByteArray allowlistAssert =
        "if _G.ants_test_extra_globals ~= '' then\n"
        "  error('non-allowlisted globals present: ' .. "
        "_G.ants_test_extra_globals)\n"
        "end\n"
        "if not _G.ants_test_os_nil then error('os not nil') end\n"
        "if not _G.ants_test_setmeta_nil then error('setmetatable not nil') end\n"
        "if not _G.ants_test_collectgc_nil then error('collectgarbage not nil') end\n"
        "if not _G.ants_test_pairs_present then error('pairs missing') end\n"
        "if not _G.ants_test_string_present then error('string missing') end\n";
    const QString allowlistAssertPath =
        writeTemp(dir, "allowlist_assert.lua", allowlistAssert);
    expect(engine.loadScript(allowlistAssertPath),
           "I7/allowlist-closes-by-default",
           QStringLiteral("after sandboxEnvironment(), _G must hold only "
                          "the allowlisted names — denied globals nil, "
                          "allowed globals present, nothing extra"));

    engine.shutdown();

    // Cleanup.
    QFile::remove(scriptPath);
    QFile::remove(assertPath);
    QFile::remove(bcPath);
    QFile::remove(probePath);
    QFile::remove(allowlistAssertPath);
    QDir().rmdir(dir);
}

void runSourceChecks() {
    const QString path = QStringLiteral(SRC_LUAENGINE_PATH);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr,
                     "[FAIL] source-open: cannot read %s\n",
                     qUtf8Printable(path));
        expect(false, "setup-error", "");
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

    // ANTS-1268 — sandbox uses an allowlist, not a denylist. The old
    // `dangerous[]` array must be gone (a denylist silently admits any
    // new global a future Lua adds), replaced by an enumerate-_G +
    // kAllowed approach.
    expect(src.contains(QStringLiteral("ANTS-1268")),
           "I7-src/ants-1268-anchor",
           QStringLiteral("luaengine.cpp must carry the ANTS-1268 "
                          "allowlist anchor"));
    expect(src.contains(QStringLiteral("kAllowed")) &&
               src.contains(QStringLiteral("lua_next(m_state")),
           "I7-src/allowlist-enumerates-_G",
           QStringLiteral("sandboxEnvironment must enumerate _G with "
                          "lua_next and keep only kAllowed names"));
    expect(!src.contains(QStringLiteral("const char *dangerous[]")),
           "I7-src/denylist-removed",
           QStringLiteral("the old dangerous[] denylist must be gone — "
                          "an allowlist replaces it"));

    // ANTS-2001 — `warn` (Lua 5.4 base lib) writes to host stderr, escaping
    // the sandbox. It must be overridden to route to ants.log, mirroring the
    // `print` redirect.
    expect(src.contains(QStringLiteral("lua_setglobal(m_state, \"warn\")")),
           "ANTS-2001/warn-redirected-to-log",
           QStringLiteral("warn must be overridden (lua_setglobal warn) so "
                          "plugin warnings flow to ants.log, not host stderr"));
}

}  // namespace

static int runMain() {
    expect_reset();
    // QCoreApplication is owned by bundle_main (ANTS-1217); this helper
    // takes no argc/argv since it doesn't need to forward to Qt.
    runRuntimeChecks();
    runSourceChecks();

    return expect_finish();
}

TEST(LuaSandboxHardening, Main) {
    ASSERT_EQ(0, runMain());
}
