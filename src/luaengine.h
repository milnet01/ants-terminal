#pragma once

#include <QString>
#include <QStringList>
#include <QObject>
#include <QHash>
#include <vector>

// Forward declare Lua state + activation record
struct lua_State;
struct lua_Debug;

// Event types for plugin hooks
enum class PluginEvent {
    Output,       // Terminal received output data
    Line,         // Complete line received
    Prompt,       // OSC 133 prompt detected
    KeyPress,     // Key pressed (before sending to PTY)
    TitleChanged, // Window title changed
    TabCreated,   // New tab created
    TabClosed,    // Tab closed
    Keybinding,   // A manifest keybinding fired — data = action id
    Load,         // Plugin loaded (fires once per plugin VM init)
    Unload,       // Plugin about to unload (save state, cleanup)
    // 0.6.9 — trigger system bundle
    CommandFinished,      // OSC 133 D — payload "exit_code=N&duration_ms=N"
    PaneFocused,          // Tab/pane focus changed — payload = tab title
    ThemeChanged,         // Theme switched — payload = theme name
    WindowConfigReloaded, // Settings applied / config.json reloaded
    UserVarChanged,       // OSC 1337;SetUserVar=NAME=value — payload "NAME=value"
    PaletteAction,        // ants.palette.register() entry triggered — payload = action id
};

// Lua scripting engine with sandboxed API.
// One instance per loaded plugin (per-plugin VM isolation) — memory budget,
// instruction hook, and event handlers all belong to this VM alone, so a
// misbehaving plugin cannot destabilize others. See PluginManager.
class LuaEngine : public QObject {
    Q_OBJECT

public:
    explicit LuaEngine(QObject *parent = nullptr);
    ~LuaEngine() override;

    // Set before initialize() to tag the VM with its plugin identity + grant
    // set. Permissions are capability strings (see PLUGINS.md). An empty
    // permission list means "default surface" (the legacy API pre-v2).
    void setPluginName(const QString &name) { m_pluginName = name; }
    const QString &pluginName() const { return m_pluginName; }
    void setPermissions(const QStringList &perms) { m_permissions = perms; }
    bool hasPermission(const QString &perm) const { return m_permissions.contains(perm); }

    bool initialize();
    bool loadScript(const QString &path);
    void shutdown();
    bool isInitialized() const { return m_state != nullptr; }

    // Memory limit for Lua allocations (default 10MB per-VM)
    static constexpr size_t MAX_LUA_MEMORY = 10 * 1024 * 1024;

    // Fire events to all registered handlers
    // Returns false if any handler requests cancellation (for keypress)
    bool fireEvent(PluginEvent event, const QString &data = QString());

    // Set context for the ants API
    void setRecentOutput(const QString &output);
    void setCwd(const QString &cwd);

signals:
    void sendToTerminal(const QString &text);
    void showNotification(const QString &title, const QString &message);
    void setStatusText(const QString &text);
    void logMessage(const QString &msg);
    // Emitted when a permissioned API is called. Handlers can perform the
    // privileged work (e.g. write the system clipboard for clipboard.write).
    void clipboardWriteRequested(const QString &text);
    // Plugin settings — per-plugin key/value with JSON-Schema backed UI.
    // Handlers (PluginManager / MainWindow) forward to the Config layer.
    void settingsGetRequested(const QString &pluginName, const QString &key, QString &outValue);
    void settingsSetRequested(const QString &pluginName, const QString &key, const QString &value);
    // ants.palette.register({title, action, hotkey}) — appends a Ctrl+Shift+P
    // entry. PluginManager forwards to MainWindow which rebuilds the palette
    // and (when hotkey is non-empty) wires a global QShortcut. action is the
    // payload echoed back via PaletteAction event when the entry fires.
    void paletteEntryRegistered(const QString &pluginName, const QString &title,
                                const QString &action, const QString &hotkey);

private:
    // ANTS-1332: instruction-count + line + wall-clock hook. Lifted from
    // a captureless lambda in initialize() to a named static so it can
    // re-arm `lua_sethook` from inside its own body once the wall budget
    // is breached (drops the count-mask threshold from 100000 to 1).
    static void instructionHook(lua_State *L, lua_Debug *ar);

    // Lua C API callbacks (static, use upvalues for 'this' pointer)
    static int lua_ants_send(lua_State *L);
    static int lua_ants_notify(lua_State *L);
    static int lua_ants_get_output(lua_State *L);
    static int lua_ants_get_cwd(lua_State *L);
    static int lua_ants_set_status(lua_State *L);
    static int lua_ants_on(lua_State *L);
    static int lua_ants_log(lua_State *L);
    static int lua_ants_clipboard_write(lua_State *L);
    static int lua_ants_settings_get(lua_State *L);
    static int lua_ants_settings_set(lua_State *L);
    static int lua_ants_palette_register(lua_State *L);

    void registerApi();
    void sandboxEnvironment();

    // Custom memory allocator with limit
    static void *luaAlloc(void *ud, void *ptr, size_t osize, size_t nsize);

    // ANTS-1172: start the wall-clock budget for the next pcall.
    // Combined with the LUA_MASKLINE | LUA_MASKCOUNT hook below, this
    // catches plugins that spend their time inside a single large C
    // call (string.gsub with a catastrophic regex, table.sort with a
    // pathological comparator) — the instruction-count hook alone
    // can't fire while pure-C is executing, so a wall-clock check at
    // the next bytecode boundary is the closest we can get to
    // bounded-duration termination on the main thread.
    void startPcallBudget();

    QString m_pluginName;
    QStringList m_permissions;
    lua_State *m_state = nullptr;
    size_t m_luaMemUsage = 0;  // Current Lua memory usage in bytes
    bool m_timedOut = false;    // Set by instruction hook, checked after pcall
    // ANTS-1332: sticky-kill latch. Set in the hook on first wall-clock
    // expiry; once true, every subsequent hook fire raises luaL_error
    // unconditionally. Cleared in startPcallBudget() (next outer pcall
    // runs at normal cadence) and in shutdown(). Header default-init
    // matches the m_timedOut pattern above.
    bool m_killed = false;
    qint64 m_pcallDeadlineMs = 0;  // ANTS-1172 — wall-clock deadline.
    QString m_recentOutput;
    QString m_cwd;

    // Event handlers: event -> list of Lua registry keys (function refs)
    QHash<PluginEvent, std::vector<int>> m_handlers;
};
