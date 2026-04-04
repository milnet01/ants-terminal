#pragma once

#include <QString>
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
};

// Lua scripting engine with sandboxed API
class LuaEngine : public QObject {
    Q_OBJECT

public:
    explicit LuaEngine(QObject *parent = nullptr);
    ~LuaEngine() override;

    bool initialize();
    bool loadScript(const QString &path);
    void shutdown();
    bool isInitialized() const { return m_state != nullptr; }

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

private:
    // Lua C API callbacks (static, use upvalues for 'this' pointer)
    static int lua_ants_send(lua_State *L);
    static int lua_ants_notify(lua_State *L);
    static int lua_ants_get_output(lua_State *L);
    static int lua_ants_get_cwd(lua_State *L);
    static int lua_ants_set_status(lua_State *L);
    static int lua_ants_on(lua_State *L);
    static int lua_ants_log(lua_State *L);

    void registerApi();
    void sandboxEnvironment();

    lua_State *m_state = nullptr;
    QString m_recentOutput;
    QString m_cwd;

    // Event handlers: event -> list of Lua registry keys (function refs)
    QHash<PluginEvent, std::vector<int>> m_handlers;
};
