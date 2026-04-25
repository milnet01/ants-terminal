#include "pluginmanager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>

#include <cstdlib>

bool PluginManager::devMode() {
    // Cached per-process: env var is read once at first call
    static const bool enabled = []() {
        const char *v = std::getenv("ANTS_PLUGIN_DEV");
        return v && v[0] && v[0] != '0';
    }();
    return enabled;
}

PluginManager::PluginManager(QObject *parent) : QObject(parent) {
    // Hot-reload watcher — only used when ANTS_PLUGIN_DEV=1. Idle cost is
    // zero because we never call watchPaths() in the non-dev path.
    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        if (devMode()) emit logMessage(QString("[plugin-dev] file changed: %1").arg(path));
        // Debounce: editors often write-truncate-write which fires twice.
        QTimer::singleShot(150, this, [this]() { reloadAll(m_watchedEnabled); });
    });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        if (devMode()) emit logMessage(QString("[plugin-dev] dir changed: %1").arg(path));
        QTimer::singleShot(150, this, [this]() { reloadAll(m_watchedEnabled); });
    });
}

PluginManager::~PluginManager() {
    unloadAll();
}

void PluginManager::setPluginDir(const QString &dir) {
    m_pluginDir = dir;
    QDir().mkpath(dir);
}

void PluginManager::unloadAll() {
    for (auto *engine : m_engines.values()) {
        if (engine) {
            // Fire unload event so plugins can save state / cleanup
            engine->fireEvent(PluginEvent::Unload, engine->pluginName());
            engine->shutdown();
            engine->deleteLater();
        }
    }
    m_engines.clear();
}

void PluginManager::wireEngine(LuaEngine *engine) {
    connect(engine, &LuaEngine::sendToTerminal, this, &PluginManager::sendToTerminal);
    connect(engine, &LuaEngine::showNotification, this, &PluginManager::showNotification);
    connect(engine, &LuaEngine::setStatusText, this, &PluginManager::statusMessage);
    connect(engine, &LuaEngine::logMessage, this, &PluginManager::logMessage);
    connect(engine, &LuaEngine::clipboardWriteRequested,
            this, &PluginManager::clipboardWriteRequested);
    connect(engine, &LuaEngine::settingsGetRequested,
            this, &PluginManager::settingsGetRequested);
    connect(engine, &LuaEngine::settingsSetRequested,
            this, &PluginManager::settingsSetRequested);
    connect(engine, &LuaEngine::paletteEntryRegistered,
            this, &PluginManager::paletteEntryRegistered);
}

void PluginManager::firePaletteAction(const QString &pluginName, const QString &actionId) {
    if (auto *engine = m_engines.value(pluginName, nullptr)) {
        engine->fireEvent(PluginEvent::PaletteAction, actionId);
    }
}

static void parseManifestInto(PluginInfo &info, const QJsonObject &obj) {
    info.name = obj["name"].toString(info.name);
    info.description = obj["description"].toString();
    info.version = obj["version"].toString("0.1.0");
    info.author = obj["author"].toString();
    if (obj.contains("permissions") && obj["permissions"].isArray()) {
        QStringList perms;
        for (const auto &v : obj["permissions"].toArray()) {
            if (v.isString()) perms << v.toString();
        }
        info.permissions = perms;
    }
    if (obj.contains("settings_schema") && obj["settings_schema"].isObject()) {
        info.settingsSchema = obj["settings_schema"].toObject();
    }
    if (obj.contains("keybindings") && obj["keybindings"].isObject()) {
        info.keybindings = obj["keybindings"].toObject();
    }
}

void PluginManager::scanAndLoad(const QStringList &enabledList) {
    unloadAll();
    m_plugins.clear();
    m_watchedEnabled = enabledList;  // remembered for hot-reload

    if (m_pluginDir.isEmpty()) return;

    // 0.7.33: anchor to the canonical path so a symlink pointing the
    // plugin directory at /etc, ~/.ssh, or anywhere else can't trick
    // the scan into loading code from outside the user's plugin tree.
    // canonicalFilePath returns "" if the path doesn't exist or
    // resolves outside readable scope; fall back to the literal path
    // in that case (the dir.exists() check below handles the
    // non-existent case identically pre- and post-canonicalization).
    QString canonicalRoot =
        QFileInfo(m_pluginDir).canonicalFilePath();
    if (canonicalRoot.isEmpty()) canonicalRoot = m_pluginDir;
    const QString canonicalRootPrefix = canonicalRoot
        + QStringLiteral("/");

    QDir dir(canonicalRoot);
    if (!dir.exists()) return;

    if (devMode()) {
        emit logMessage(QString("[plugin-dev] scanning %1 (enabled=%2)")
                        .arg(canonicalRoot, enabledList.join(',')));
    }

    // Scan for plugin directories containing init.lua. NoSymLinks
    // rejects sub-entries whose name resolves through a symlink
    // (the per-entry canonical-path check below catches more exotic
    // cases — bind mounts, hardlinks under different names — but
    // NoSymLinks is the cheap first-pass filter).
    QStringList dirs = dir.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);

    // Reset watcher (removes old paths)
    if (devMode() && m_watcher) {
        if (!m_watcher->files().isEmpty()) m_watcher->removePaths(m_watcher->files());
        if (!m_watcher->directories().isEmpty()) m_watcher->removePaths(m_watcher->directories());
    }

    for (const QString &pluginDirName : dirs) {
        QString pluginPath = canonicalRoot + "/" + pluginDirName;

        // 0.7.33: confirm the entry's canonical path is anchored
        // INSIDE the canonical plugin root. NoSymLinks above filters
        // most escape patterns; this check picks up the residue
        // (entry that exists at scan time but resolves outside via a
        // bind mount, a chain through a non-symlink the kernel has
        // remapped, etc.). Reject silently — a plugin tree shouldn't
        // contain anything that needs canonicalization beyond a
        // single component.
        const QString canonicalPlugin =
            QFileInfo(pluginPath).canonicalFilePath();
        if (canonicalPlugin.isEmpty() ||
            !(canonicalPlugin == canonicalRoot ||
              canonicalPlugin.startsWith(canonicalRootPrefix))) {
            qWarning("Ants: plugin entry %s resolves outside the plugin "
                     "root (%s) — skipped",
                     qUtf8Printable(pluginPath),
                     qUtf8Printable(canonicalRoot));
            continue;
        }

        QString initLua = canonicalPlugin + "/init.lua";

        if (!QFile::exists(initLua)) continue;

        PluginInfo info;
        info.path = canonicalPlugin;
        info.name = pluginDirName;

        // Read optional manifest.json
        QString manifestPath = canonicalPlugin + "/manifest.json";
        if (QFile::exists(manifestPath)) {
            QFile f(manifestPath);
            if (f.open(QIODevice::ReadOnly)) {
                // 0.7.33: cap manifest size before reading. Pre-fix
                // `f.readAll()` allocated whatever the on-disk file
                // claimed it had — a 4 GB manifest would OOM the
                // process before QJsonDocument::fromJson got a chance
                // to reject it. 1 MiB is ~250 plugins worth of
                // permissions/description text; legitimate manifests
                // are <10 KiB.
                constexpr qint64 kMaxManifestBytes = 1024 * 1024;  // 1 MiB
                if (f.size() > kMaxManifestBytes) {
                    qWarning("Ants: plugin manifest %s exceeds %lld-byte cap "
                             "(%lld bytes on disk) — skipped",
                             qUtf8Printable(manifestPath),
                             static_cast<long long>(kMaxManifestBytes),
                             static_cast<long long>(f.size()));
                } else {
                    QJsonParseError err{};
                    QJsonDocument doc = QJsonDocument::fromJson(
                        f.read(kMaxManifestBytes), &err);
                    if (doc.isObject()) {
                        parseManifestInto(info, doc.object());
                    } else {
                        qWarning("Ants: plugin manifest %s failed to parse: %s (byte offset %d)",
                                 qUtf8Printable(manifestPath),
                                 qUtf8Printable(err.errorString()), err.offset);
                    }
                }
            }
        }

        info.enabled = enabledList.isEmpty() || enabledList.contains(pluginDirName);
        m_plugins.append(info);

        if (info.enabled) {
            loadPlugin(info);
            if (devMode() && m_watcher) {
                // Watch init.lua + manifest.json for edits
                m_watcher->addPath(initLua);
                if (QFile::exists(manifestPath)) m_watcher->addPath(manifestPath);
                m_watcher->addPath(canonicalPlugin);
            }
        }
    }

    emit pluginsReloaded();
}

void PluginManager::reloadAll(const QStringList &enabledList) {
    scanAndLoad(enabledList);
}

void PluginManager::loadPlugin(const PluginInfo &info) {
    // Permission acceptance. If the manifest declares permissions and the user
    // hasn't granted them yet (per the GrantStore callback), prompt via
    // PermissionPrompt. Deny by default if no prompt wired.
    QStringList granted;
    if (!info.permissions.isEmpty()) {
        QStringList saved = m_grantLoad ? m_grantLoad(info.name) : QStringList{};
        // If any requested permission isn't already granted, re-prompt the user.
        bool needsPrompt = false;
        for (const auto &p : info.permissions) {
            if (!saved.contains(p)) { needsPrompt = true; break; }
        }
        if (needsPrompt && m_permissionPrompt) {
            granted = m_permissionPrompt(info, info.permissions);
            if (m_grantSave) m_grantSave(info.name, granted);
        } else {
            granted = saved;
        }
    }

    auto *engine = new LuaEngine(this);
    engine->setPluginName(info.name);
    engine->setPermissions(granted);
    wireEngine(engine);

    if (!engine->initialize()) {
        emit logMessage(QString("Failed to initialize VM for plugin: %1").arg(info.name));
        engine->deleteLater();
        return;
    }

    QString initLua = info.path + "/init.lua";
    if (devMode()) {
        emit logMessage(QString("[plugin-dev] loading %1 from %2 (granted: %3)")
                        .arg(info.name, initLua, granted.join(',')));
    }
    if (!engine->loadScript(initLua)) {
        emit logMessage(QString("Failed to load plugin: %1").arg(info.name));
        engine->shutdown();
        engine->deleteLater();
        return;
    }

    m_engines.insert(info.name, engine);
    // Fire load event now that the script has registered its handlers
    engine->fireEvent(PluginEvent::Load, info.name);
    emit logMessage(QString("Loaded plugin: %1 v%2").arg(info.name, info.version));
}

bool PluginManager::fireEvent(PluginEvent event, const QString &data) {
    bool allow = true;
    // Snapshot engines — a plugin could register a new handler mid-fire.
    auto snapshot = m_engines.values();
    for (auto *engine : snapshot) {
        if (!engine) continue;
        if (!engine->fireEvent(event, data)) allow = false;
    }
    return allow;
}

void PluginManager::setRecentOutput(const QString &output) {
    for (auto *engine : m_engines.values()) {
        if (engine) engine->setRecentOutput(output);
    }
}

void PluginManager::setCwd(const QString &cwd) {
    for (auto *engine : m_engines.values()) {
        if (engine) engine->setCwd(cwd);
    }
}
