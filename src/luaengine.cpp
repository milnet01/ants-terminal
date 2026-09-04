#include "luaengine.h"

#include "pathvalidation.h"

// Unqualified on purpose (ANTS-3727): the directory comes from
// LUA_INCLUDE_DIRS, which CMakeLists.txt puts on this target's include path.
// openSUSE ships the headers in /usr/include/lua5.4, Fedora in /usr/include —
// so the <lua5.4/...> prefix that worked here resolved only on openSUSE and
// broke the Fedora build even though CMake had found Lua correctly.
#include <lua.hpp>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// Helper to retrieve LuaEngine* from Lua state upvalue
static LuaEngine *getEngine(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__ants_engine");
    auto *engine = static_cast<LuaEngine *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return engine;
}

// Panic handler. PLUGINS.md guarantees the terminal will never crash
// from a plugin; the per-plugin `lua_State`'s default panic handler
// calls `abort()`, which would breach that contract on any unhandled
// Lua error escaping a `pcall` (today: rare; tomorrow: a regression
// adding a `lua_call` site). Log + return 0 — Lua treats a non-jump
// return from atpanic as fatal anyway, so the process still aborts,
// but at least the user sees an actionable diagnostic instead of a
// bare SIGABRT.
static int luaPanicHandler(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    qCritical().nospace()
        << "ants: Lua plugin VM panic (unhandled error outside pcall): "
        << (msg ? msg : "<no message>");
    return 0;
}

// ANTS-4442 — push a handler and its argument from INSIDE a protected call.
//
// The panic handler above concedes that a non-jump return from atpanic is
// still fatal, and its comment assumed unprotected allocation sites were
// hypothetical. fireEvent had two. lua_pushlstring allocates; at the heap
// budget the capped allocator returns NULL, Lua raises a memory error, and
// with no protected frame the process aborts — breaching PLUGINS.md's "The
// terminal will never crash because of your plugin", for a state that same
// document describes as one a plugin merely lives in.
//
// Setting the protected frame up cannot itself raise: lua_checkstack
// reports failure by returning 0, and pushing a C function with no
// upvalues, or a light userdata, stores a value directly without
// allocating a Lua object.
struct HandlerPushCtx {
    int         ref;
    const char *data;
    size_t      len;
};

static int luaPushHandlerAndArg(lua_State *L) {
    auto *ctx = static_cast<HandlerPushCtx *>(lua_touserdata(L, 1));
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ref);
    lua_pushlstring(L, ctx->data, ctx->len);
    return 2;   // the handler, then its string argument
}

// Custom Lua allocator with memory limit (prevents string.rep OOM)
void *LuaEngine::luaAlloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    auto *engine = static_cast<LuaEngine *>(ud);
    if (nsize == 0) {
        // Free — guard against unsigned underflow from accounting drift
        if (osize <= engine->m_luaMemUsage)
            engine->m_luaMemUsage -= osize;
        else
            engine->m_luaMemUsage = 0;
        free(ptr);
        return nullptr;
    }
    // Per Lua 5.4 manual: when ptr == NULL, `osize` encodes the OBJECT TYPE
    // being allocated (LUA_TSTRING/TABLE/etc., a small int 0..8), not a
    // byte count. Treating it as bytes drifts m_luaMemUsage downward on
    // every fresh allocation, silently letting plugins exceed MAX_LUA_MEMORY.
    if (ptr == nullptr) osize = 0;
    // Check memory limit on allocate/realloc. Guard the subtraction against
    // unsigned underflow the same way the free path does — if accounting has
    // drifted such that osize > tracked usage, a wrap would spuriously deny a
    // legit shrink or (worse) let a plugin slip past the cap. indie-review-2026-05-21.
    const size_t base =
        (osize <= engine->m_luaMemUsage) ? engine->m_luaMemUsage - osize : 0;
    size_t newTotal = base + nsize;
    if (newTotal > MAX_LUA_MEMORY) {
        return nullptr; // Lua treats NULL return as allocation failure
    }
    void *result = realloc(ptr, nsize);
    if (result) {
        engine->m_luaMemUsage = newTotal;
    }
    return result;
}

static bool eventFromString(const char *name, PluginEvent &out) {
    if (strcmp(name, "output") == 0) { out = PluginEvent::Output; return true; }
    if (strcmp(name, "line") == 0) { out = PluginEvent::Line; return true; }
    if (strcmp(name, "prompt") == 0) { out = PluginEvent::Prompt; return true; }
    if (strcmp(name, "keypress") == 0) { out = PluginEvent::KeyPress; return true; }
    if (strcmp(name, "title_changed") == 0) { out = PluginEvent::TitleChanged; return true; }
    if (strcmp(name, "tab_created") == 0) { out = PluginEvent::TabCreated; return true; }
    if (strcmp(name, "tab_closed") == 0) { out = PluginEvent::TabClosed; return true; }
    if (strcmp(name, "keybinding") == 0) { out = PluginEvent::Keybinding; return true; }
    if (strcmp(name, "load") == 0) { out = PluginEvent::Load; return true; }
    if (strcmp(name, "unload") == 0) { out = PluginEvent::Unload; return true; }
    // 0.6.9 — trigger system bundle
    if (strcmp(name, "command_finished") == 0) { out = PluginEvent::CommandFinished; return true; }
    if (strcmp(name, "pane_focused") == 0) { out = PluginEvent::PaneFocused; return true; }
    if (strcmp(name, "theme_changed") == 0) { out = PluginEvent::ThemeChanged; return true; }
    if (strcmp(name, "window_config_reloaded") == 0) { out = PluginEvent::WindowConfigReloaded; return true; }
    if (strcmp(name, "user_var_changed") == 0) { out = PluginEvent::UserVarChanged; return true; }
    if (strcmp(name, "palette_action") == 0) { out = PluginEvent::PaletteAction; return true; }
    return false;
}

LuaEngine::LuaEngine(QObject *parent) : QObject(parent) {}

LuaEngine::~LuaEngine() {
    shutdown();
}

bool LuaEngine::initialize() {
    // INV-1 — the lua_State is created and only ever touched on the owning
    // worker thread. In the non-threaded unit tests thread() == the calling
    // thread, so this also holds there.
    Q_ASSERT(thread() == QThread::currentThread());
    if (m_state) return true;
    return buildVm(/*queryMode=*/false, QString());
}

// ANTS-2093 — shared VM construction. The plugin path (initialize) and the
// query path (runQuery) both come through here so the sandbox setup — the
// 5-lib allowlist, the memory allocator, the instruction/wall-clock hook —
// has ONE definition and the two can't drift apart on a future hardening
// fix. The ONLY difference is which API table is installed: registerApi()
// (the write-capable ants.*) for a plugin, registerQueryApi() (read-only
// project.*) for a query.
bool LuaEngine::buildVm(bool queryMode, const QString &queryRoot) {
    m_luaMemUsage = 0;
    m_timedOut = false;
    m_killed = false;
    m_state = lua_newstate(luaAlloc, this);
    if (!m_state) return false;
    lua_atpanic(m_state, &luaPanicHandler);

    // Load safe standard libraries
    luaL_requiref(m_state, "string", luaopen_string, 1); lua_pop(m_state, 1);
    luaL_requiref(m_state, "table", luaopen_table, 1); lua_pop(m_state, 1);
    luaL_requiref(m_state, "math", luaopen_math, 1); lua_pop(m_state, 1);
    luaL_requiref(m_state, "utf8", luaopen_utf8, 1); lua_pop(m_state, 1);
    luaL_requiref(m_state, "_G", luaopen_base, 1); lua_pop(m_state, 1);

    // Store engine pointer in registry
    lua_pushlightuserdata(m_state, this);
    lua_setfield(m_state, LUA_REGISTRYINDEX, "__ants_engine");

    // Register our API — write-capable ants.* (plugin) or read-only
    // project.* (query). ANTS-2093: a query VM never sees registerApi()'s
    // side-effecting callbacks (INV-1).
    if (queryMode)
        registerQueryApi(queryRoot);
    else
        registerApi();

    // Sandbox: remove dangerous functions
    sandboxEnvironment();

    // ANTS-1172: instruction-count + per-line + wall-clock watchdog.
    // The instruction-count hook (100k instructions) bounds pure-Lua
    // busy loops; the line hook fires per Lua source line so a tight
    // C-call-in-loop pattern (e.g. `for i=1,N do string.find(...) end`)
    // hits the deadline check more often than once per 100k ops; the
    // wall-clock check inside the hook closes the case where a plugin
    // burns time across mostly-C surfaces and the instruction count
    // alone wouldn't accumulate fast enough to fire before a freeze
    // is felt by the user. A SINGLE C call (one giant gsub) still
    // can't be preempted on the main thread — the budget is enforced
    // at the next bytecode boundary.
    //
    // ANTS-1332: the hook callback is the named static
    // LuaEngine::instructionHook so it can re-arm lua_sethook from
    // inside its own body once the wall budget is breached.
    lua_sethook(m_state, &LuaEngine::instructionHook,
                LUA_MASKCOUNT | LUA_MASKLINE, 100000);

    return true;
}

void LuaEngine::instructionHook(lua_State *L, lua_Debug * /*ar*/) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__ants_engine");
    auto *eng = static_cast<LuaEngine *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!eng) {
        luaL_error(L, "Script execution timeout exceeded");
        return;
    }
    // ANTS-1750 — GUI-set abort. The controller sets m_abortRequested when
    // it demotes a plugin whose handler ran past budget + grace; raise at
    // the next hook fire (bounded by the 100k-instruction / per-line
    // cadence, i.e. sub-millisecond for pure-Lua / C-in-loop runaways). It
    // does nothing for a single uninterruptible C call — the hook is dormant
    // then — which is exactly why PluginManager's detach path exists. INV-5.
    if (eng->m_abortRequested.load(std::memory_order_relaxed)) {
        luaL_error(L, "Plugin aborted by host (unresponsive handler)");
        return;
    }
    // ANTS-1332 — sticky-kill latch. Once a wall-clock timeout has
    // fired for this pcall, every subsequent hook fire raises
    // luaL_error unconditionally. A Lua-level inner pcall in the
    // plugin will still catch the longjmp, but with the count-mask
    // threshold dropped to 1 below (see "first expiry" branch) the
    // plugin gets at most one VM instruction or one line-change
    // between catches. Loop-nested attacks unwind through the
    // engine's outer lua_pcall the moment the post-catch instruction
    // is outside any inner pcall; source-nested attacks unwind one
    // frame at a time until LUAI_MAXCCALLS bounds the depth.
    if (eng->m_killed) {
        luaL_error(L, "Script wall-clock budget exceeded (latched)");
        return;
    }
    // ANTS-2205 — monotonic elapsed vs budget (was epoch-ms now > deadline).
    // !isValid() == unarmed (startPcallBudget not yet called).
    const bool wallExpired =
        eng->m_pcallBudgetMs > 0 && eng->m_pcallTimer.isValid()
        && eng->m_pcallTimer.elapsed() > eng->m_pcallBudgetMs;
    if (wallExpired) {
        eng->m_timedOut = true;
        eng->m_killed = true;
        // Re-arm with count=1 BEFORE luaL_error — luaL_error longjmps
        // and any statement after it is unreachable (ANTS-1332
        // Invariants 3 + 5). Keep both mask bits set so line-changes
        // still trip the hook in the unlikely case the plugin
        // spans multiple source lines between count-1 fires.
        lua_sethook(L, &LuaEngine::instructionHook,
                    LUA_MASKCOUNT | LUA_MASKLINE, 1);
        luaL_error(L, "Script wall-clock budget exceeded");
    }
}

void LuaEngine::startPcallBudget() {
    if (!m_state) return;
    // ANTS-1332 — clear the sticky-kill latch and restore the normal
    // count-mask threshold before the next pcall runs. Cheap on the
    // well-behaved-plugin hot path (one bool store + one lua_sethook
    // call, both touching the same lua_State cache line).
    m_killed = false;
    lua_sethook(m_state, &LuaEngine::instructionHook,
                LUA_MASKCOUNT | LUA_MASKLINE, 100000);
    // Default 1.5 s per pcall — big enough to let a normal plugin
    // finish (most fire-and-forget handlers complete in single-digit
    // ms), small enough that the user notices a stall as "snappy
    // still" rather than "frozen." `m_pcallBudgetMs` is tunable via
    // `setPcallBudgetMs()` for tests that exercise the kill path —
    // production callers never touch it.
    m_pcallTimer.start();  // ANTS-2205 — monotonic budget clock
}

void LuaEngine::registerApi() {
    // Create 'ants' table
    lua_newtable(m_state);

    // --- Always-on surface (no permission required) ---
    // ants.send(text)
    lua_pushcfunction(m_state, lua_ants_send);
    lua_setfield(m_state, -2, "send");

    // ants.notify(title, message)
    lua_pushcfunction(m_state, lua_ants_notify);
    lua_setfield(m_state, -2, "notify");

    // ants.get_output(n)
    lua_pushcfunction(m_state, lua_ants_get_output);
    lua_setfield(m_state, -2, "get_output");

    // ants.get_cwd()
    lua_pushcfunction(m_state, lua_ants_get_cwd);
    lua_setfield(m_state, -2, "get_cwd");

    // ants.set_status(text)
    lua_pushcfunction(m_state, lua_ants_set_status);
    lua_setfield(m_state, -2, "set_status");

    // ants.on(event, callback)
    lua_pushcfunction(m_state, lua_ants_on);
    lua_setfield(m_state, -2, "on");

    // ants.log(message)
    lua_pushcfunction(m_state, lua_ants_log);
    lua_setfield(m_state, -2, "log");

    // ants._version — terminal version string (lets plugins feature-detect)
    lua_pushstring(m_state, ANTS_VERSION);
    lua_setfield(m_state, -2, "_version");

    // ants._plugin_name — plugin's declared name (manifest.json "name")
    lua_pushstring(m_state, m_pluginName.toUtf8().constData());
    lua_setfield(m_state, -2, "_plugin_name");

    // --- Permissioned surface ---
    // Each capability exposes one or more functions gated by the plugin's
    // manifest "permissions" array. Functions absent from the env when the
    // permission is missing (not stubbed with nil) so plugins can feature-
    // detect with `if ants.clipboard then ...`.

    // clipboard.write — requires "clipboard.write"
    if (hasPermission("clipboard.write")) {
        lua_newtable(m_state);
        lua_pushcfunction(m_state, lua_ants_clipboard_write);
        lua_setfield(m_state, -2, "write");
        lua_setfield(m_state, -2, "clipboard");
    }

    // settings.get / settings.set — requires "settings"
    if (hasPermission("settings")) {
        lua_newtable(m_state);
        lua_pushcfunction(m_state, lua_ants_settings_get);
        lua_setfield(m_state, -2, "get");
        lua_pushcfunction(m_state, lua_ants_settings_set);
        lua_setfield(m_state, -2, "set");
        lua_setfield(m_state, -2, "settings");
    }

    // palette.register({title, action, hotkey}) — always-on. Adds a UI entry
    // visible only via the user's existing Ctrl+Shift+P, no privileged
    // capability is granted; rejected at registration if the entry shape is
    // invalid. Hotkey wiring happens in MainWindow (QShortcut). The plugin
    // receives a `palette_action` event with `action` as payload when fired.
    {
        lua_newtable(m_state);
        lua_pushcfunction(m_state, lua_ants_palette_register);
        lua_setfield(m_state, -2, "register");
        lua_setfield(m_state, -2, "palette");
    }

    lua_setglobal(m_state, "ants");
}

void LuaEngine::sandboxEnvironment() {
    // ANTS-1268 — allowlist the globals rather than denylisting them.
    // A denylist silently admits any NEW global a future Lua release
    // adds (Lua 5.4 already added `warn`; a later patch could add
    // more), so the sandbox must close by default: enumerate `_G` and
    // nil every name that is NOT on the documented-safe allowlist
    // (PLUGINS.md § Sandbox Boundaries). The kept set is exactly the
    // previously-allowed surface — the base safe globals + the four
    // stdlib tables loaded above + our `ants` API — so no current
    // plugin breaks, and the removed set still covers everything the
    // old denylist did (os/io/load*/raw*/*metatable/collectgarbage/
    // require/package/debug/coroutine).
    static const char *const kAllowed[] = {
        // base safe globals (Lua 5.4 luaopen_base, minus the unsafe)
        "_G", "_VERSION", "assert", "error", "ipairs", "next",
        "pairs", "pcall", "print", "select", "tonumber", "tostring",
        "type", "warn", "xpcall",
        // safe standard libraries loaded above
        "string", "table", "math", "utf8",
        // Ants plugin API
        "ants",
        // ANTS-2093 — read-only query API (registerQueryApi). A no-op in
        // plugin mode (no `project` global exists there); kept so the query
        // VM's `project` table survives the _G purge.
        "project",
        nullptr,
    };
    auto isAllowed = [&](const char *name) {
        for (int i = 0; kAllowed[i]; ++i)
            if (qstrcmp(name, kAllowed[i]) == 0) return true;
        return false;
    };
    // Collect the disallowed names first — modifying `_G` while
    // iterating it with lua_next is fragile (setting existing fields
    // to nil mid-traversal is permitted, but collect-then-remove is
    // unambiguous). The LUA_TSTRING guard keeps lua_tostring from
    // mutating a numeric key in place (which would corrupt lua_next).
    QList<QByteArray> toRemove;
    lua_getglobal(m_state, "_G");
    lua_pushnil(m_state);
    while (lua_next(m_state, -2) != 0) {
        if (lua_type(m_state, -2) == LUA_TSTRING) {
            const char *name = lua_tostring(m_state, -2);
            if (name && !isAllowed(name)) toRemove.append(QByteArray(name));
        }
        lua_pop(m_state, 1);  // pop value, keep key for the next lua_next
    }
    lua_pop(m_state, 1);      // pop _G
    for (const QByteArray &name : toRemove) {
        lua_pushnil(m_state);
        lua_setglobal(m_state, name.constData());
    }

    // Strip string.dump. It returns the bytecode for a Lua function;
    // Lua 5.4 has no bytecode verifier, so crafted bytecode can corrupt
    // memory and escape the sandbox. `load`/`loadstring`/`loadfile` are
    // already nil'd above, so there's no supported path back from
    // bytecode to an executing function — but any future C API added
    // to `ants.*` that calls luaL_loadbuffer on plugin-supplied data
    // would reopen the attack surface. Defense-in-depth: close the
    // primitive at the sandbox layer where the rule is checked, not
    // at the API call site where the rule is easy to forget.
    lua_getglobal(m_state, "string");
    if (lua_istable(m_state, -1)) {
        lua_pushnil(m_state);
        lua_setfield(m_state, -2, "dump");
    }
    lua_pop(m_state, 1);

    // Indie-review-2026-05-14 lane-6 M-3: redirect `print` to ants.log.
    // PLUGINS.md § Sandbox Boundaries says "print() is typically
    // redirected to ants.log" — but pre-fix nothing actually
    // overrode the base-lib `print`, so plugin output went to the
    // host process's stdout (terminal-emulator stdout or systemd
    // journal). Information-disclosure surface is small but the doc
    // contract was wrong. Override with the same C function ants.log
    // uses so plugin print() flows into the structured log.
    lua_pushcfunction(m_state, lua_ants_log);
    lua_setglobal(m_state, "print");

    // ANTS-2001 — the Lua 5.4 base-lib `warn` writes to the host process's
    // stderr, escaping the sandbox (same information-disclosure shape the
    // `print` redirect above closes; PLUGINS.md § Sandbox Boundaries promises
    // plugin diagnostics go to ants.log). Override it to route to the
    // structured log too.
    lua_pushcfunction(m_state, lua_ants_warn);
    lua_setglobal(m_state, "warn");
}

bool LuaEngine::loadScript(const QString &path) {
    Q_ASSERT(thread() == QThread::currentThread());  // INV-1
    if (!m_state) return false;

    // Reject compiled bytecode — Lua 5.4 has no bytecode verifier, so
    // crafted bytecode can corrupt memory and escape the sandbox.
    QFile check(path);
    if (check.open(QIODevice::ReadOnly)) {
        char first = 0;
        if (check.read(&first, 1) == 1 && first == '\x1b') {
            emit logMessage(QString("Rejected binary bytecode: %1").arg(path));
            return false;
        }
        check.close();
    }

    // luaL_dofile is luaL_loadfile + lua_pcall; luaL_loadfile forwards to
    // luaL_loadfilex with mode nullptr, which accepts BOTH text and binary
    // bytecode. The 0x1b peek above is our first gate, but duplicate the
    // check at the Lua-loader level by forcing mode "t" (text-only). A
    // future refactor that drops the peek still gets a rejection here.
    const QByteArray pathUtf8 = path.toUtf8();
    int result = luaL_loadfilex(m_state, pathUtf8.constData(), "t");
    if (result == LUA_OK) {
        startPcallBudget();
        result = lua_pcall(m_state, 0, LUA_MULTRET, 0);
    }
    if (result != LUA_OK) {
        const char *err = lua_tostring(m_state, -1);
        emit logMessage(QString("Lua error in %1: %2").arg(path, err ? err : "unknown"));
        lua_pop(m_state, 1);
        // ANTS-1750 § 2.2.2 — a half-loaded VM must not receive the queued
        // Load event. Tear it down here, on the worker, so the FIFO-following
        // Load no-ops on fireEvent's null-m_state guard and lua_close stays
        // off the GUI thread (INV-12). In the threaded flow a failed init.lua
        // means a dead plugin; recovery semantics (ANTS-1332) live on the
        // per-handler pcall path inside fireEvent, not on re-loading a chunk.
        shutdown();
        return false;
    }
    return true;
}

void LuaEngine::shutdown() {
    if (m_state) {
        m_handlers.clear();
        // Clear the instruction-count hook before lua_close runs.
        // lua_close executes every __gc metamethod in dependency order;
        // metamethods can run arbitrary Lua code, which the count hook
        // observes. If the hook fires mid-close and walks back into
        // registry data that has already been finalized (or into the
        // LuaEngine pointer whose Qt signal connections are tearing
        // down), we get a UAF window. Clearing the hook first makes
        // lua_close purely C-side cleanup from the Ants perspective.
        lua_sethook(m_state, nullptr, 0, 0);
        lua_close(m_state);
        m_state = nullptr;
        m_luaMemUsage = 0;
        m_timedOut = false;
        m_killed = false;
    }
}

bool LuaEngine::fireEvent(PluginEvent event, const QString &data) {
    if (!m_state) return true;

    auto it = m_handlers.find(event);
    if (it == m_handlers.end()) return true;

    // A handler may call ants.on(), which can rehash m_handlers (new event ->
    // every value relocates) or reallocate this vector (same event); either
    // dangles the range-for's cached begin/end. Iterate a copy (ANTS-4441).
    const std::vector<int> handlers = it.value();

    bool allow = true;
    QByteArray dataUtf8 = data.toUtf8();

    for (int ref : handlers) {
        // ANTS-4442 — both pushes happen inside luaPushHandlerAndArg's
        // protected call. Done here directly they run outside any pcall, and
        // a plugin sitting at its documented heap budget takes the terminal
        // down with it. Only stack space is secured unprotected, which
        // lua_checkstack reports rather than raises.
        if (!lua_checkstack(m_state, 4)) {
            emit logMessage(
                QStringLiteral("Plugin error: out of Lua stack — event "
                               "handler skipped"));
            continue;
        }
        HandlerPushCtx ctx{ ref, dataUtf8.constData(),
                            static_cast<size_t>(dataUtf8.size()) };
        lua_pushcfunction(m_state, &luaPushHandlerAndArg);
        lua_pushlightuserdata(m_state, &ctx);
        if (lua_pcall(m_state, 1, 2, 0) != LUA_OK) {
            const char *perr = lua_tostring(m_state, -1);
            emit logMessage(QStringLiteral("Plugin error: %1")
                                .arg(perr ? QString::fromUtf8(perr)
                                          : QStringLiteral(
                                                "out of memory preparing "
                                                "event handler")));
            lua_pop(m_state, 1);
            continue;
        }

        startPcallBudget();
        if (lua_pcall(m_state, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(m_state, -1);
            emit logMessage(QString("Plugin error: %1").arg(err ? err : "unknown"));
            lua_pop(m_state, 1);

            // If timed out, stop all handler execution (pcall cannot escape timeout)
            if (m_timedOut) {
                emit logMessage("Plugin timed out — execution stopped");
                m_timedOut = false;
                break;
            }
            continue;
        }

        // Check return value: if false, cancel the event
        if (lua_isboolean(m_state, -1) && !lua_toboolean(m_state, -1)) {
            allow = false;
        }
        lua_pop(m_state, 1);

        // Also check timeout after successful pcall (script may have caught it internally)
        if (m_timedOut) {
            emit logMessage("Plugin timed out — execution stopped");
            m_timedOut = false;
            break;
        }
    }

    return allow;
}

void LuaEngine::dispatchEvent(int event, const QString &data) {
    // INV-1 — Lua C API must only run on the owning worker thread.
    Q_ASSERT(thread() == QThread::currentThread());
    const quint64 seq = ++m_seq;
    // eventStarted/eventCompleted bracket the actual handler execution so
    // PluginManager's health tick measures handler runtime (not queue wait):
    // a worker stuck in one uninterruptible C call emits eventStarted then
    // never eventCompleted, ageing past budget + grace; a backed-up but
    // healthy worker keeps pairing them per event, so it is not demoted.
    emit eventStarted(seq);
    fireEvent(static_cast<PluginEvent>(event), data);
    emit eventCompleted(seq);
}

void LuaEngine::setRecentOutput(const QString &output) {
    m_recentOutput = output;
}

void LuaEngine::setCwd(const QString &cwd) {
    m_cwd = cwd;
}

// --- Lua C API callbacks ---

int LuaEngine::lua_ants_send(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *text = luaL_checkstring(L, 1);
    if (engine && text) {
        emit engine->sendToTerminal(QString::fromUtf8(text));
    }
    return 0;
}

int LuaEngine::lua_ants_notify(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *title = luaL_checkstring(L, 1);
    const char *message = luaL_optstring(L, 2, "");
    if (engine) {
        emit engine->showNotification(QString::fromUtf8(title), QString::fromUtf8(message));
    }
    return 0;
}

// ANTS-1802 — push a QString as a length-counted Lua string. lua_pushstring
// is NUL-terminated C-string semantics: it truncates at the first embedded
// '\0'. Terminal output (m_recentOutput) is attacker-influenced, so a single
// NUL byte in the shell's output would silently blind any plugin scanning
// get_output. lua_pushlstring carries the exact byte count.
static void pushQString(lua_State *L, const QString &s) {
    const QByteArray ba = s.toUtf8();
    lua_pushlstring(L, ba.constData(), static_cast<size_t>(ba.size()));
}

int LuaEngine::lua_ants_get_output(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    if (engine) {
        int n = luaL_optinteger(L, 1, 50);
        if (n < 0) n = 0;  // negative count would over-run lines.mid()
        QStringList lines = engine->m_recentOutput.split('\n');
        if (lines.size() > n) {
            lines = lines.mid(lines.size() - n);
        }
        pushQString(L, lines.join('\n'));
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

int LuaEngine::lua_ants_get_cwd(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    if (engine) {
        pushQString(L, engine->m_cwd);
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

int LuaEngine::lua_ants_set_status(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *text = luaL_checkstring(L, 1);
    if (engine) {
        emit engine->setStatusText(QString::fromUtf8(text));
    }
    return 0;
}

int LuaEngine::lua_ants_on(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (engine) {
        PluginEvent event;
        if (!eventFromString(eventName, event))
            return luaL_error(L, "unknown event: %s", eventName);
        // Per-event handler cap. A plugin that called ants.on(event, fn)
        // in a loop would grow this list until the heap cap fired —
        // a real cap here surfaces the bug directly. 64 handlers per
        // event is far above any legitimate plugin's needs.
        constexpr int kMaxHandlersPerEvent = 64;
        auto &handlers = engine->m_handlers[event];
        if (handlers.size() >= kMaxHandlersPerEvent) {
            return luaL_error(L, "ants.on: too many handlers for event %s "
                              "(cap %d)", eventName, kMaxHandlersPerEvent);
        }
        // Store function reference in registry
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        handlers.push_back(ref);
    }
    return 0;
}

int LuaEngine::lua_ants_log(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *msg = luaL_checkstring(L, 1);
    if (engine) {
        emit engine->logMessage(QString::fromUtf8(msg));
    }
    return 0;
}

int LuaEngine::lua_ants_warn(lua_State *L) {
    // Mirror of lua_ants_log for the base-lib `warn` (ANTS-2001). Lua 5.4
    // `warn(msg1, ...)` takes one-or-more string args; a first arg starting
    // with '@' is a control message ("@on"/"@off"), not output. Swallow
    // control messages (warnings always log here, no on/off state) and
    // concatenate the rest into one ants.log line — never the host stderr.
    const int n = lua_gettop(L);
    if (n >= 1) {
        const char *first = lua_tostring(L, 1);
        if (first && first[0] == '@')
            return 0;  // control message — ignore
    }
    LuaEngine *engine = getEngine(L);
    if (!engine) return 0;
    QString msg;
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);  // coerce + push
        msg += QString::fromUtf8(s, static_cast<int>(len));
        lua_pop(L, 1);  // pop the string luaL_tolstring pushed
    }
    emit engine->logMessage(msg);
    return 0;
}

int LuaEngine::lua_ants_clipboard_write(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *text = luaL_checkstring(L, 1);
    if (engine && engine->hasPermission("clipboard.write")) {
        emit engine->clipboardWriteRequested(QString::fromUtf8(text));
    }
    return 0;
}

int LuaEngine::lua_ants_settings_get(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *key = luaL_checkstring(L, 1);
    if (engine && engine->hasPermission("settings")) {
        QString out;
        emit engine->settingsGetRequested(engine->pluginName(),
                                           QString::fromUtf8(key), out);
        if (out.isNull()) {
            lua_pushnil(L);
        } else {
            pushQString(L, out);  // ANTS-1802 — NUL-safe length-counted push
        }
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int LuaEngine::lua_ants_settings_set(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *key = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);
    if (engine && engine->hasPermission("settings")) {
        emit engine->settingsSetRequested(engine->pluginName(),
                                           QString::fromUtf8(key),
                                           QString::fromUtf8(value));
    }
    return 0;
}

int LuaEngine::lua_ants_palette_register(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    // Pull title / action / hotkey out of the table — all strings, hotkey
    // optional. action is the payload echoed back via PaletteAction event;
    // title is the visible label; hotkey is a Qt key sequence string.
    lua_getfield(L, 1, "title");
    const char *title = lua_tostring(L, -1);
    lua_getfield(L, 1, "action");
    const char *action = lua_tostring(L, -1);
    lua_getfield(L, 1, "hotkey");
    const char *hotkey = lua_tostring(L, -1);

    if (!title || !title[0] || !action || !action[0]) {
        lua_pop(L, 3);
        return luaL_error(L, "ants.palette.register requires non-empty 'title' and 'action'");
    }

    if (engine) {
        emit engine->paletteEntryRegistered(
            engine->pluginName(),
            QString::fromUtf8(title),
            QString::fromUtf8(action),
            hotkey ? QString::fromUtf8(hotkey) : QString());
    }
    lua_pop(L, 3);
    return 0;
}

// =====================================================================
// ANTS-2093 — project_query: server-side read-only Lua snippet runner.
// =====================================================================

namespace {

// Minimal strict UTF-8 validator (rejects overlongs, surrogates,
// > U+10FFFF, truncated sequences). Lua strings are raw byte arrays, so
// a snippet can build or `project.read` a non-UTF-8 string; the JSON
// result must be valid UTF-8, so an invalid one is refused at marshal
// time (INV-6) rather than silently lossy-converted to U+FFFD.
bool isValidUtf8(const char *s, size_t len) {
    const auto *p = reinterpret_cast<const unsigned char *>(s);
    size_t i = 0;
    while (i < len) {
        const unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int n;                              // continuation-byte count
        unsigned char lo = 0x80, hi = 0xBF; // bounds for the 1st cont. byte
        if ((c & 0xE0) == 0xC0) { n = 1; if (c < 0xC2) return false; }
        else if ((c & 0xF0) == 0xE0) { n = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if ((c & 0xF8) == 0xF0) { n = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; if (c > 0xF4) return false; }
        else return false;
        if (i + static_cast<size_t>(n) >= len) return false;  // truncated
        for (int k = 1; k <= n; ++k) {
            const unsigned char cc = p[i + k];
            const unsigned char rlo = (k == 1) ? lo : 0x80;
            const unsigned char rhi = (k == 1) ? hi : 0xBF;
            if (cc < rlo || cc > rhi) return false;
        }
        i += static_cast<size_t>(n) + 1;
    }
    return true;
}

// §2.4 marshalling. Depth-bounded at 32 levels — also what stops a
// circular table (t.self=t) from looping the encoder (caught by the
// bound, not a separate cycle check). `err` is sticky: once set, every
// recursive call short-circuits and the caller refuses with query_error.
constexpr int kMarshalMaxDepth = 32;

QJsonValue marshalLuaValue(lua_State *L, int idx, int depth, QString &err);

QJsonValue marshalLuaTable(lua_State *L, int idx, int depth, QString &err) {
    // Bound only TABLE nesting (a scalar leaf at any depth is harmless).
    // A table at level > 32 refuses — this is also what stops a circular
    // table (t.self=t) from looping the encoder: each recursion bumps depth
    // until the bound trips, no separate cycle detection needed.
    if (depth > kMarshalMaxDepth) {
        err = QStringLiteral("table nested deeper than %1 levels").arg(kMarshalMaxDepth);
        return {};
    }
    // Each nesting level keeps a key+value pair on the Lua value stack while
    // recursing, so a deep (or circular) table can exhaust the default stack
    // — ensure headroom rather than push blindly into undefined behaviour.
    if (!lua_checkstack(L, 4)) {
        err = QStringLiteral("marshal: Lua stack exhausted");
        return {};
    }
    idx = lua_absindex(L, idx);
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));

    // Array-like iff every key is an integer in 1..n and the key count
    // equals n (no holes, no extra string keys).
    bool arrayLike = true;
    lua_Integer keyCount = 0;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        ++keyCount;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            const lua_Integer k = lua_tointeger(L, -2);
            if (k < 1 || k > n) arrayLike = false;
        } else {
            arrayLike = false;
        }
        lua_pop(L, 1);  // pop value, keep key for next lua_next
    }

    if (arrayLike && keyCount == n) {
        QJsonArray arr;
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            arr.append(marshalLuaValue(L, -1, depth + 1, err));
            lua_pop(L, 1);
            if (!err.isEmpty()) return {};
        }
        return arr;
    }

    // Otherwise a string-keyed object; a non-string key refuses (INV-6).
    QJsonObject obj;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 2);  // pop value + key, abandon iteration
            err = QStringLiteral("table has a non-string key");
            return {};
        }
        size_t klen = 0;
        const char *ks = lua_tolstring(L, -2, &klen);  // key is a string ⇒ no mutation
        if (!isValidUtf8(ks, klen)) {
            lua_pop(L, 2);
            err = QStringLiteral("object key is not valid UTF-8");
            return {};
        }
        const QString key = QString::fromUtf8(ks, static_cast<int>(klen));
        const QJsonValue v = marshalLuaValue(L, -1, depth + 1, err);
        lua_pop(L, 1);  // pop value, keep key for next lua_next
        if (!err.isEmpty()) { lua_pop(L, 1); return {}; }
        obj.insert(key, v);
    }
    return obj;
}

QJsonValue marshalLuaValue(lua_State *L, int idx, int depth, QString &err) {
    if (!err.isEmpty()) return {};
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
        case LUA_TNONE:
            return QJsonValue(QJsonValue::Null);
        case LUA_TBOOLEAN:
            return QJsonValue(static_cast<bool>(lua_toboolean(L, idx)));
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx))
                return QJsonValue(static_cast<qint64>(lua_tointeger(L, idx)));
            else {
                const double d = lua_tonumber(L, idx);
                if (!std::isfinite(d)) {  // NaN/Inf has no JSON form (would serialise to null)
                    err = QStringLiteral("non-finite number");
                    return {};
                }
                return QJsonValue(d);
            }
        case LUA_TSTRING: {
            size_t len = 0;
            const char *s = lua_tolstring(L, idx, &len);
            if (!isValidUtf8(s, len)) {
                err = QStringLiteral("string is not valid UTF-8");
                return {};
            }
            return QJsonValue(QString::fromUtf8(s, static_cast<int>(len)));
        }
        case LUA_TTABLE:
            return marshalLuaTable(L, idx, depth, err);
        default:  // function / userdata / thread
            err = QStringLiteral("unsupported return type");
            return {};
    }
}

// Serialise a marshalled value to its compact UTF-8 bytes for the output
// cap (§2.4). QJsonDocument only wraps object/array, so a scalar is wrapped
// in a one-element array and the enclosing brackets stripped.
QByteArray serializeJsonValue(const QJsonValue &v) {
    if (v.isObject())
        return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray())
        return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    const QByteArray wrapped =
        QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);  // strip "[" … "]"
}

// §2.4 — detached query workers held until process exit. A worker stuck in
// an uninterruptible C call owns its lua_State frame and can't be safely
// joined or freed, so the thread (and its captured heap result slot) leaks
// by design (RAM consequence in spec §4). Appended ONLY from the
// single-threaded MCP dispatch thread, so no lock is needed — the only
// theoretical race is re-entrancy via a nested-loop dispatch, which this path
// never triggers (ANTS-2194).
QVector<QThread *> g_queryZombies;
// ANTS-2194 — hard ceiling on leaked query workers. A wedged worker can never be
// reaped, so an unbounded wedge-loop would leak ~512 KiB/thread until OOM. Past
// this cap runQueryThreaded refuses new threaded queries (and logs the count) so
// the leak is bounded and the pathology observable rather than silent. 64 ×
// 512 KiB ≈ 32 MiB — well past any healthy steady state, short of run-away.
constexpr int kMaxQueryZombies = 64;

}  // namespace

void LuaEngine::registerQueryApi(const QString &root) {
    m_queryRoot = root;
    lua_newtable(m_state);
    lua_pushcfunction(m_state, lua_project_read);  lua_setfield(m_state, -2, "read");
    lua_pushcfunction(m_state, lua_project_list);  lua_setfield(m_state, -2, "list");
    lua_pushcfunction(m_state, lua_project_root);  lua_setfield(m_state, -2, "root");
    lua_setglobal(m_state, "project");
}

int LuaEngine::lua_project_read(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const char *rel = luaL_checkstring(L, 1);
    if (!engine) return luaL_error(L, "project.read: no engine");
    const auto chk = PathValidation::validatePath(
        QString::fromUtf8(rel), engine->m_queryRoot,
        QStringLiteral("project_query"), QStringLiteral("path"));
    if (chk.bad) return luaL_error(L, "project.read: \"%s\" escapes project root", rel);
    if (chk.resolved.isEmpty()) return luaL_error(L, "project.read: no such file: %s", rel);
    QFile f(chk.resolved);
    if (!f.open(QIODevice::ReadOnly)) return luaL_error(L, "project.read: cannot open %s", rel);
    const QByteArray data = f.readAll();  // VM 10 MiB cap bounds the push below
    lua_pushlstring(L, data.constData(), static_cast<size_t>(data.size()));
    return 1;
}

int LuaEngine::lua_project_list(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    if (!engine) return luaL_error(L, "project.list: no engine");
    QString base = engine->m_queryRoot;
    if (lua_gettop(L) >= 1 && !lua_isnoneornil(L, 1)) {
        const char *sub = luaL_checkstring(L, 1);
        const auto chk = PathValidation::validatePath(
            QString::fromUtf8(sub), engine->m_queryRoot,
            QStringLiteral("project_query"), QStringLiteral("subdir"));
        if (chk.bad) return luaL_error(L, "project.list: \"%s\" escapes project root", sub);
        if (chk.resolved.isEmpty() || !QFileInfo(chk.resolved).isDir())
            return luaL_error(L, "project.list: not a directory: %s", sub);
        base = chk.resolved;
    }
    // Enumerate regular files (incl. dotfiles like .gitignore — project
    // content), skipping only .git/ (internal metadata; perf on big repos).
    const QDir rootDir(engine->m_queryRoot);
    // ANTS-2203 — canonical root for the per-entry containment check below.
    const QString canonRoot = QFileInfo(engine->m_queryRoot).canonicalFilePath();
    QList<QByteArray> rels;
    QDirIterator it(base,
                    QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = rootDir.relativeFilePath(abs);
        if (rel == QStringLiteral(".git") ||
            rel.startsWith(QStringLiteral(".git/")) ||
            rel.contains(QStringLiteral("/.git/")))
            continue;
        // ANTS-2203 — re-validate each entry's canonical path against the root so
        // a symlink (leaf file OR an ancestor dir QDirIterator followed) whose
        // target escapes the project is not disclosed even by name. project.read
        // already canonicalises+validates, so this keeps list ⊆ read-acceptable;
        // an empty canonical path (broken/escaping link) is skipped.
        const QString canon = QFileInfo(abs).canonicalFilePath();
        if (canon.isEmpty() ||
            (canon != canonRoot &&
             !canon.startsWith(canonRoot + QLatin1Char('/'))))
            continue;
        rels.append(rel.toUtf8());
    }
    // Sort by raw UTF-8 bytes (QByteArray operator< is memcmp) — codepoint
    // order, locale-independent, so identical snapshots yield identical
    // bytes regardless of the runner's collation (INV-7; guards ANTS-2120).
    std::sort(rels.begin(), rels.end());
    lua_createtable(L, static_cast<int>(rels.size()), 0);
    int i = 1;
    for (const QByteArray &r : rels) {
        lua_pushlstring(L, r.constData(), static_cast<size_t>(r.size()));
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int LuaEngine::lua_project_root(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    const QByteArray r = engine ? engine->m_queryRoot.toUtf8() : QByteArray();
    lua_pushlstring(L, r.constData(), static_cast<size_t>(r.size()));
    return 1;
}

LuaEngine::QueryResult LuaEngine::runQuery(const QString &code, const QString &root,
                                           qint64 timeoutMs, int resultCapBytes) {
    Q_ASSERT(thread() == QThread::currentThread());  // INV-1 — own thread only
    QueryResult qr;
    QElapsedTimer timer;
    timer.start();

    if (m_state) shutdown();  // fresh VM per call (INV-5) even on a reused engine
    setPcallBudgetMs(timeoutMs);
    if (!buildVm(/*queryMode=*/true, root)) {
        qr.code  = QStringLiteral("query_error");
        qr.error = QStringLiteral("project_query: VM construction failed");
        return qr;
    }

    auto fail = [&](const QString &codeStr, const QString &msg) -> QueryResult {
        qr.ok = false; qr.code = codeStr; qr.error = msg;
        qr.elapsedMs = timer.elapsed();
        shutdown();
        return qr;
    };

    // Load text-only (mode "t") — mirrors loadScript's bytecode rejection.
    const QByteArray codeUtf8 = code.toUtf8();
    int lr = luaL_loadbufferx(m_state, codeUtf8.constData(),
                              static_cast<size_t>(codeUtf8.size()),
                              "=project_query", "t");
    if (lr != LUA_OK) {
        const char *e = lua_tostring(m_state, -1);
        const QString msg = QStringLiteral("project_query: %1")
                                .arg(e ? QString::fromUtf8(e) : QStringLiteral("load error"));
        return fail(lr == LUA_ERRMEM ? QStringLiteral("query_oom")
                                     : QStringLiteral("query_error"), msg);
    }

    startPcallBudget();
    const int pr = lua_pcall(m_state, 0, 1, 0);  // exactly one result (§2.4)
    qr.elapsedMs = timer.elapsed();
    if (pr != LUA_OK) {
        const char *e = lua_tostring(m_state, -1);
        const QString msg = QStringLiteral("project_query: %1")
                                .arg(e ? QString::fromUtf8(e) : QStringLiteral("runtime error"));
        // m_timedOut (set by the wall-clock hook) wins over the LUA_ERRMEM
        // status so a budget kill never misreports as OOM.
        QString refusal = QStringLiteral("query_error");
        if (m_timedOut)              refusal = QStringLiteral("query_timeout");
        else if (pr == LUA_ERRMEM)   refusal = QStringLiteral("query_oom");
        return fail(refusal, msg);
    }

    // Marshal the single top-of-stack value.
    QString merr;
    const QJsonValue value = marshalLuaValue(m_state, lua_gettop(m_state), 1, merr);
    if (!merr.isEmpty())
        return fail(QStringLiteral("query_error"),
                    QStringLiteral("project_query: %1").arg(merr));

    const QByteArray bytes = serializeJsonValue(value);
    if (bytes.size() > resultCapBytes)
        return fail(QStringLiteral("result_too_large"),
                    QStringLiteral("project_query: result is %1 bytes (cap %2) — "
                                   "aggregate further (return a count, not the rows)")
                        .arg(bytes.size()).arg(resultCapBytes));

    qr.ok = true;
    qr.result = value;
    shutdown();
    return qr;
}

LuaEngine::QueryResult LuaEngine::runQueryThreaded(const QString &code, const QString &root,
                                                   qint64 timeoutMs, int resultCapBytes) {
    // ANTS-2194 — back-pressure: if too many prior workers are already wedged,
    // refuse rather than spawn yet another doomed (unreapable) thread. Bounds the
    // leak under a pathological wedge-loop. Only ever read/written on the single
    // dispatch thread (same precondition as g_queryZombies.append below).
    if (g_queryZombies.size() >= kMaxQueryZombies) {
        QueryResult out;
        out.ok    = false;
        out.code  = QStringLiteral("query_error");
        out.error = QStringLiteral("project_query: %1 query workers already wedged "
                                   "(cap %2) — refusing new threaded query")
                        .arg(g_queryZombies.size()).arg(kMaxQueryZombies);
        return out;
    }

    // Heap result slot: the worker may outlive this call (detach path), so
    // it must NOT write into a stack local. `code`/`root` are copied into
    // the worker's functor (each thread owns its QString refs).
    auto *slot = new QueryResult();
    const QString codeCopy = code;
    const QString rootCopy = root;
    QThread *worker = QThread::create([slot, codeCopy, rootCopy, timeoutMs, resultCapBytes]() {
        LuaEngine eng;  // QObject affinity = this worker thread
        *slot = eng.runQuery(codeCopy, rootCopy, timeoutMs, resultCapBytes);
    });
    worker->setObjectName(QStringLiteral("ants-project-query"));
    worker->setStackSize(512 * 1024);  // shallow sandbox; matches plugin workers
    worker->start();

    if (worker->wait(timeoutMs + kQueryJoinGraceMs)) {
        const QueryResult out = *slot;
        delete worker;
        delete slot;
        return out;
    }

    // Detached — the snippet is stuck in an uninterruptible C call past the
    // join deadline. Leak worker + slot (the worker still owns them); the
    // GUI/dispatch thread resumes now with query_timeout.
    g_queryZombies.append(worker);
    // ANTS-2194 — log the running zombie count so a wedge-loop is observable
    // (a steadily climbing number is the signal a snippet class reliably hangs).
    qWarning("project_query: detached wedged worker; %d now leaked (cap %d)",
             static_cast<int>(g_queryZombies.size()), kMaxQueryZombies);
    QueryResult out;
    out.ok    = false;
    out.code  = QStringLiteral("query_timeout");
    out.error = QStringLiteral("project_query: snippet exceeded %1 ms (detached)")
                    .arg(timeoutMs);
    return out;
}

QJsonObject LuaEngine::projectQueryVerb(const QJsonObject &req, bool enabled,
                                        qint64 timeoutMs, int resultCapBytes) {
    auto refuse = [](const QString &code, const QString &msg) {
        QJsonObject o;
        o[QStringLiteral("ok")]    = false;
        o[QStringLiteral("code")]  = code;
        o[QStringLiteral("error")] = msg;
        return o;
    };

    // §2.5 — feature gate is the FIRST statement, before any arg validation,
    // so a feature-off call yields query_disabled regardless of arg state.
    if (!enabled)
        return refuse(QStringLiteral("query_disabled"),
                      QStringLiteral("project_query is disabled (Settings → General → "
                                     "\"Let Ants run read-only project queries for Claude\")"));

    const QString rawCaller = req.value(QStringLiteral("caller_cwd")).toString();
    const QString root = QFileInfo(rawCaller).canonicalFilePath();
    if (root.isEmpty() || !QFileInfo(root).isDir())
        return refuse(QStringLiteral("cwd_bad"),
                      QStringLiteral("project_query: caller_cwd \"%1\" is not a directory")
                          .arg(rawCaller));

    const QString code = req.value(QStringLiteral("code")).toString();
    if (code.isEmpty())
        return refuse(QStringLiteral("missing_field"),
                      QStringLiteral("project_query: \"code\" is required (a read-only "
                                     "Lua snippet that returns a value)"));

    const QueryResult qr = runQueryThreaded(code, root, timeoutMs, resultCapBytes);
    if (!qr.ok)
        return refuse(qr.code, qr.error);

    QJsonObject o;
    o[QStringLiteral("ok")]         = true;
    o[QStringLiteral("result")]     = qr.result;
    o[QStringLiteral("elapsed_ms")] = qr.elapsedMs;
    return o;
}
