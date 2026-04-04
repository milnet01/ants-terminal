#include "luaengine.h"

#include <lua5.4/lua.hpp>
#include <QDebug>

// Helper to retrieve LuaEngine* from Lua state upvalue
static LuaEngine *getEngine(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "__ants_engine");
    auto *engine = static_cast<LuaEngine *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return engine;
}

static PluginEvent eventFromString(const char *name) {
    if (strcmp(name, "output") == 0) return PluginEvent::Output;
    if (strcmp(name, "line") == 0) return PluginEvent::Line;
    if (strcmp(name, "prompt") == 0) return PluginEvent::Prompt;
    if (strcmp(name, "keypress") == 0) return PluginEvent::KeyPress;
    if (strcmp(name, "title_changed") == 0) return PluginEvent::TitleChanged;
    if (strcmp(name, "tab_created") == 0) return PluginEvent::TabCreated;
    if (strcmp(name, "tab_closed") == 0) return PluginEvent::TabClosed;
    return PluginEvent::Output; // default
}

LuaEngine::LuaEngine(QObject *parent) : QObject(parent) {}

LuaEngine::~LuaEngine() {
    shutdown();
}

bool LuaEngine::initialize() {
    if (m_state) return true;

    m_state = luaL_newstate();
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
    lua_sethook(m_state, [](lua_State *L, lua_Debug *) {
        luaL_error(L, "Script execution timeout exceeded");
    }, LUA_MASKCOUNT, 10000000);

    return true;
}

void LuaEngine::registerApi() {
    // Create 'ants' table
    lua_newtable(m_state);

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

    lua_setglobal(m_state, "ants");
}

void LuaEngine::sandboxEnvironment() {
    // Remove dangerous globals
    const char *dangerous[] = {
        "os", "io", "loadfile", "dofile", "load",
        "rawget", "rawset", "rawequal", "rawlen",
        "collectgarbage", "require", "package", "debug",
        nullptr
    };
    for (int i = 0; dangerous[i]; ++i) {
        lua_pushnil(m_state);
        lua_setglobal(m_state, dangerous[i]);
    }
}

bool LuaEngine::loadScript(const QString &path) {
    if (!m_state) return false;

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
            continue;
        }

        // Check return value: if false, cancel the event
        if (lua_isboolean(m_state, -1) && !lua_toboolean(m_state, -1)) {
            allow = false;
        }
        lua_pop(m_state, 1);
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
        PluginEvent event = eventFromString(eventName);
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
