#pragma once

#include <QString>
#include <QStringList>
#include <QObject>
#include <QHash>
#include <functional>
#include <string>
#include <vector>

// Forward declare Lua state
struct lua_State;

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

    QString m_pluginName;
    QStringList m_permissions;
    lua_State *m_state = nullptr;
    size_t m_luaMemUsage = 0;  // Current Lua memory usage in bytes
    bool m_timedOut = false;    // Set by instruction hook, checked after pcall
    QString m_recentOutput;
    QString m_cwd;

    // Event handlers: event -> list of Lua registry keys (function refs)
    QHash<PluginEvent, std::vector<int>> m_handlers;
};
