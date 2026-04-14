#pragma once

#include "luaengine.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QJsonObject>

class QFileSystemWatcher;

struct PluginInfo {
    QString name;
    QString description;
    QString version;
    QString author;
    QString path;          // Directory path
    QStringList permissions; // Manifest v2 "permissions" array
    QJsonObject settingsSchema; // Manifest v2 "settings_schema" (JSON Schema subset)
    QJsonObject keybindings; // Manifest v2 "keybindings" { "action-id": "Ctrl+Shift+X", ... }
    bool enabled = true;
};

// Discovers, loads, and manages Lua plugins
class PluginManager : public QObject {
    Q_OBJECT

public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager() override;

    // ANTS_PLUGIN_DEV=1 enables verbose logging + hot reload for plugin authors
    static bool devMode();

    void setPluginDir(const QString &dir);
    QString pluginDir() const { return m_pluginDir; }

    // Scan for plugins and load enabled ones
    void scanAndLoad(const QStringList &enabledList);
    void reloadAll(const QStringList &enabledList);

    // Plugin list
    const QList<PluginInfo> &plugins() const { return m_plugins; }
    int pluginCount() const { return m_plugins.size(); }

    // Forward events to every loaded plugin VM. Events cancel (return false)
    // if ANY plugin's handler returns false (for keypress interception).
    bool fireEvent(PluginEvent event, const QString &data = QString());

    // Update context for all plugins
    void setRecentOutput(const QString &output);
    void setCwd(const QString &cwd);

    // Access to the engine for a given plugin (nullptr if not loaded)
    LuaEngine *engineFor(const QString &pluginName) const {
        return m_engines.value(pluginName, nullptr);
    }

    // Load-time acceptance callback: invoked when a plugin requests a
    // non-empty permission set and hasn't been granted in config yet.
    // Return the subset the user accepted (empty list to deny all).
    using PermissionPrompt = std::function<QStringList(const PluginInfo &info,
                                                       const QStringList &requested)>;
    void setPermissionPrompt(PermissionPrompt p) { m_permissionPrompt = std::move(p); }

    // Fire a palette_action event to a specific plugin's VM. Used by MainWindow
    // when the user triggers a plugin-registered palette entry (via click or
    // shortcut). Targeted, not broadcast — only the registering plugin sees it.
    void firePaletteAction(const QString &pluginName, const QString &actionId);
    // Persisted per-plugin grants (plugin -> permission list). Called back by
    // the permission prompt and used for subsequent loads without asking.
    using GrantStore = std::function<QStringList(const QString &pluginName)>;
    using GrantSave  = std::function<void(const QString &pluginName,
                                          const QStringList &granted)>;
    void setGrantStore(GrantStore load, GrantSave save) {
        m_grantLoad = std::move(load);
        m_grantSave = std::move(save);
    }

signals:
    void sendToTerminal(const QString &text);
    void showNotification(const QString &title, const QString &message);
    void statusMessage(const QString &msg);
    void logMessage(const QString &msg);
    void clipboardWriteRequested(const QString &text);
    // Forwarded from engines — MainWindow wires these to the Config store
    void settingsGetRequested(const QString &pluginName, const QString &key, QString &out);
    void settingsSetRequested(const QString &pluginName, const QString &key, const QString &value);
    void pluginsReloaded();
    // 0.6.9 — ants.palette.register() forward. MainWindow rebuilds the
    // Ctrl+Shift+P palette + (when hotkey is non-empty) wires a QShortcut
    // that calls back into PluginManager::firePaletteAction(plugin, action).
    void paletteEntryRegistered(const QString &pluginName, const QString &title,
                                const QString &action, const QString &hotkey);

private:
    void loadPlugin(const PluginInfo &info);
    void unloadAll();
    void wireEngine(LuaEngine *engine);

    QString m_pluginDir;
    QList<PluginInfo> m_plugins;
    QMap<QString, LuaEngine *> m_engines;  // keyed by plugin name
    QFileSystemWatcher *m_watcher = nullptr;
    QStringList m_watchedEnabled;  // cached enabled list for hot-reload

    PermissionPrompt m_permissionPrompt;
    GrantStore m_grantLoad;
    GrantSave m_grantSave;
};
