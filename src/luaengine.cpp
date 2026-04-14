#include "luaengine.h"

#include <lua5.4/lua.hpp>
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
    // Check memory limit on allocate/realloc
    size_t newTotal = engine->m_luaMemUsage - osize + nsize;
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
    m_state = lua_newstate(luaAlloc, this);
    if (!m_state) return false;

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

    // Set instruction count hook for timeout (10 million instructions)
    // Sets m_timedOut flag so pcall cannot silently swallow the error
    lua_sethook(m_state, [](lua_State *L, lua_Debug *) {
        lua_getfield(L, LUA_REGISTRYINDEX, "__ants_engine");
        auto *eng = static_cast<LuaEngine *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (eng) eng->m_timedOut = true;
        luaL_error(L, "Script execution timeout exceeded");
    }, LUA_MASKCOUNT, 10000000);

    return true;
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

    int result = luaL_dofile(m_state, path.toUtf8().constData());
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
        lua_close(m_state);
        m_state = nullptr;
        m_luaMemUsage = 0;
        m_timedOut = false;
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
        // Store function reference in registry
        lua_pushvalue(L, 2);
        int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        engine->m_handlers[event].push_back(ref);
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
