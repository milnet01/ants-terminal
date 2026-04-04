#include "pluginmanager.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

PluginManager::PluginManager(QObject *parent) : QObject(parent) {
    m_engine = new LuaEngine(this);
    connect(m_engine, &LuaEngine::sendToTerminal, this, &PluginManager::sendToTerminal);
    connect(m_engine, &LuaEngine::showNotification, this, &PluginManager::showNotification);
    connect(m_engine, &LuaEngine::setStatusText, this, &PluginManager::statusMessage);
    connect(m_engine, &LuaEngine::logMessage, this, &PluginManager::logMessage);
}

PluginManager::~PluginManager() {
    if (m_engine) m_engine->shutdown();
}

void PluginManager::setPluginDir(const QString &dir) {
    m_pluginDir = dir;
    QDir().mkpath(dir);
}

void PluginManager::scanAndLoad(const QStringList &enabledList) {
    m_plugins.clear();

    if (m_pluginDir.isEmpty()) return;

    QDir dir(m_pluginDir);
    if (!dir.exists()) return;

    // Initialize Lua engine
    if (!m_engine->isInitialized()) {
        if (!m_engine->initialize()) {
            emit logMessage("Failed to initialize Lua engine");
            return;
        }
    }

    // Scan for plugin directories containing init.lua
    QStringList dirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pluginDirName : dirs) {
        QString pluginPath = m_pluginDir + "/" + pluginDirName;
        QString initLua = pluginPath + "/init.lua";

        if (!QFile::exists(initLua)) continue;

        PluginInfo info;
        info.path = pluginPath;
        info.name = pluginDirName;

        // Read optional manifest.json
        QString manifestPath = pluginPath + "/manifest.json";
        if (QFile::exists(manifestPath)) {
            QFile f(manifestPath);
            if (f.open(QIODevice::ReadOnly)) {
                QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
                if (doc.isObject()) {
                    QJsonObject obj = doc.object();
                    info.name = obj["name"].toString(pluginDirName);
                    info.description = obj["description"].toString();
                    info.version = obj["version"].toString("0.1.0");
                    info.author = obj["author"].toString();
                }
            }
        }

        // Check if enabled
        info.enabled = enabledList.isEmpty() || enabledList.contains(pluginDirName);
        m_plugins.append(info);

        if (info.enabled) {
            loadPlugin(info);
        }
    }
}

void PluginManager::reloadAll(const QStringList &enabledList) {
    m_engine->shutdown();
    scanAndLoad(enabledList);
}

void PluginManager::loadPlugin(const PluginInfo &info) {
    QString initLua = info.path + "/init.lua";
    if (!m_engine->loadScript(initLua)) {
        emit logMessage(QString("Failed to load plugin: %1").arg(info.name));
    } else {
        emit logMessage(QString("Loaded plugin: %1 v%2").arg(info.name, info.version));
    }
}

bool PluginManager::fireEvent(PluginEvent event, const QString &data) {
    return m_engine->fireEvent(event, data);
}

void PluginManager::setRecentOutput(const QString &output) {
    m_engine->setRecentOutput(output);
}

void PluginManager::setCwd(const QString &cwd) {
    m_engine->setCwd(cwd);
}
