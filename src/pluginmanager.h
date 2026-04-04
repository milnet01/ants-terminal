#pragma once

#include "luaengine.h"

#include <QObject>
#include <QString>
#include <QList>

struct PluginInfo {
    QString name;
    QString description;
    QString version;
    QString author;
    QString path;       // Directory path
    bool enabled = true;
};

// Discovers, loads, and manages Lua plugins
class PluginManager : public QObject {
    Q_OBJECT

public:
    explicit PluginManager(QObject *parent = nullptr);
    ~PluginManager() override;

    void setPluginDir(const QString &dir);
    QString pluginDir() const { return m_pluginDir; }

    // Scan for plugins and load enabled ones
    void scanAndLoad(const QStringList &enabledList);
    void reloadAll(const QStringList &enabledList);

    // Plugin list
    const QList<PluginInfo> &plugins() const { return m_plugins; }
    int pluginCount() const { return m_plugins.size(); }

    // Forward events to the Lua engine
    bool fireEvent(PluginEvent event, const QString &data = QString());

    // Update context for plugins
    void setRecentOutput(const QString &output);
    void setCwd(const QString &cwd);

    LuaEngine *engine() { return m_engine; }

signals:
    void sendToTerminal(const QString &text);
    void showNotification(const QString &title, const QString &message);
    void statusMessage(const QString &msg);
    void logMessage(const QString &msg);

private:
    void loadPlugin(const PluginInfo &info);

    QString m_pluginDir;
    QList<PluginInfo> m_plugins;
    LuaEngine *m_engine = nullptr;
};
