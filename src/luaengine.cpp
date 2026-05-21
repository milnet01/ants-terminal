#include "luaengine.h"

#include <lua5.4/lua.hpp>
#include <QDateTime>
#include <QDebug>
#include <QFile>
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
    if (m_state) return true;

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

    // Register our API
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
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool wallExpired =
        eng->m_pcallDeadlineMs > 0 && nowMs > eng->m_pcallDeadlineMs;
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
    m_pcallDeadlineMs = QDateTime::currentMSecsSinceEpoch() + m_pcallBudgetMs;
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
    // Remove dangerous globals (getmetatable allows string metatable manipulation)
    const char *dangerous[] = {
        "os", "io", "loadfile", "dofile", "load",
        "rawget", "rawset", "rawequal", "rawlen",
        "setmetatable", "getmetatable", "collectgarbage",
        "require", "package", "debug", "coroutine",
        nullptr
    };
    for (int i = 0; dangerous[i]; ++i) {
        lua_pushnil(m_state);
        lua_setglobal(m_state, dangerous[i]);
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
}

bool LuaEngine::loadScript(const QString &path) {
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

    bool allow = true;
    QByteArray dataUtf8 = data.toUtf8();

    for (int ref : it.value()) {
        lua_rawgeti(m_state, LUA_REGISTRYINDEX, ref);
        lua_pushstring(m_state, dataUtf8.constData());

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

int LuaEngine::lua_ants_get_output(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    if (engine) {
        int n = luaL_optinteger(L, 1, 50);
        QStringList lines = engine->m_recentOutput.split('\n');
        if (lines.size() > n) {
            lines = lines.mid(lines.size() - n);
        }
        lua_pushstring(L, lines.join('\n').toUtf8().constData());
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

int LuaEngine::lua_ants_get_cwd(lua_State *L) {
    LuaEngine *engine = getEngine(L);
    if (engine) {
        lua_pushstring(L, engine->m_cwd.toUtf8().constData());
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
            lua_pushstring(L, out.toUtf8().constData());
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
