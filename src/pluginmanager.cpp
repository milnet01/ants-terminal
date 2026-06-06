#include "pluginmanager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QThread>
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
        // ANTS-1788 — never reload outside dev mode. Inert today (no
        // paths are watched in the non-dev path) but a future watch
        // added outside dev mode would otherwise trigger a surprise
        // reloadAll teardown of live plugins.
        if (!devMode()) return;
        emit logMessage(QString("[plugin-dev] file changed: %1").arg(path));
        // Debounce: editors often write-truncate-write which fires twice.
        QTimer::singleShot(150, this, [this]() { reloadAll(m_watchedEnabled); });
    });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        if (!devMode()) return;  // ANTS-1788 — see fileChanged above
        emit logMessage(QString("[plugin-dev] dir changed: %1").arg(path));
        QTimer::singleShot(150, this, [this]() { reloadAll(m_watchedEnabled); });
    });

    // ANTS-1750 — self-owned 2 s health tick. Each plugin VM runs on its own
    // worker thread; the GUI controller cannot interrupt a single
    // uninterruptible C call mid-flight, so it detects one here (execution
    // time past budget + grace) and demotes the plugin to observe-only. This
    // is deliberately PluginManager's own timer, not MainWindow's status
    // timer (which the controller can't reach). See healthTick().
    m_healthTimer = new QTimer(this);
    m_healthTimer->setInterval(2000);
    connect(m_healthTimer, &QTimer::timeout, this, &PluginManager::healthTick);
    m_healthTimer->start();
}

PluginManager::~PluginManager() {
    unloadAll();
}

void PluginManager::setPluginDir(const QString &dir) {
    m_pluginDir = dir;
    QDir().mkpath(dir);
}

void PluginManager::unloadAll() {
    // ANTS-1750 — tear down each worker. teardownEngine posts Unload +
    // shutdown to the worker (FIFO), then quit + wait(kTeardownMs); a worker
    // stuck in an uninterruptible C call is detached into m_zombies rather
    // than joined (INV-4) and every lua_close stays on the worker (INV-12).
    // The ANTS-1173 re-entrancy UAF is gone for free: an Unload handler now
    // runs on the worker and can only signal the GUI via queued connections,
    // so it cannot synchronously re-enter fireEvent against a half-torn-down
    // m_engines.
    const auto names = m_engines.keys();
    for (const QString &name : names) {
        teardownEngine(name, m_engines.value(name, nullptr),
                       m_threads.value(name, nullptr));
    }
    m_engines.clear();
    m_threads.clear();
    m_execStart.clear();
    m_demoted.clear();
}

void PluginManager::teardownEngine(const QString &name, LuaEngine *engine,
                                   QThread *thread) {
    Q_UNUSED(name);
    if (!engine && !thread) return;
    if (!thread) {            // engine without a worker — should not happen
        delete engine;        // safe: never moved, lives on the GUI thread
        return;
    }
    if (engine) {
        // FIFO on the worker: run the Unload handler, then shutdown
        // (lua_close on the worker — INV-12), then quit the worker's own
        // event loop from inside it so the two posted calls are guaranteed
        // to run first. The quit is posted to the ENGINE (worker affinity),
        // not to the QThread (GUI affinity) — posting to the thread object
        // would queue onto the GUI loop, which is not running during wait().
        QMetaObject::invokeMethod(engine, "dispatchEvent", Qt::QueuedConnection,
                                  Q_ARG(int, int(PluginEvent::Unload)),
                                  Q_ARG(QString, engine->pluginName()));
        QMetaObject::invokeMethod(engine, "shutdown", Qt::QueuedConnection);
        QMetaObject::invokeMethod(engine, [thread]() { thread->quit(); },
                                  Qt::QueuedConnection);
    } else {
        thread->quit();
    }

    if (thread->wait(kTeardownMs)) {
        // Clean exit — the worker drained Unload + shutdown (m_state now
        // null) and quit. Deleting the engine on the GUI thread is safe now
        // the worker has finished; ~LuaEngine's shutdown() is a no-op (no
        // lua_close on the GUI thread). INV-11 / INV-12.
        // ANTS-1803 — drop the health-bookkeeping keys BEFORE delete so a
        // reused heap address can't make the next-loaded engine alias a stale
        // "demoted" entry (the keys are raw LuaEngine* pointers).
        m_demoted.remove(engine);
        m_execStart.remove(engine);
        delete engine;
        delete thread;
    } else {
        // Worker stuck in an uninterruptible C call — detach. Do NOT delete
        // or shutdown (its lua_State is owned by the stuck C frame); hold the
        // pair until process exit. The thread is parentless so PM destruction
        // never deletes a still-running QThread. INV-4.
        // ANTS-1804 — sever the zombie's signals to this PluginManager first.
        // A worker that returns from its stuck C call after detach would
        // otherwise deliver queued sendToTerminal/eventCompleted/etc. into a
        // PluginManager that may already be destroyed (it is parented to
        // MainWindow, torn down at app exit while zombies are never joined).
        QObject::disconnect(engine, nullptr, this, nullptr);
        m_demoted.remove(engine);
        m_execStart.remove(engine);
        m_zombies.append(Zombie{thread, engine});
        emit logMessage(QString("Plugin worker did not stop in %1ms — "
                                "detached: %2")
                            .arg(kTeardownMs)
                            .arg(engine ? engine->pluginName() : QString()));
    }
}

void PluginManager::wireEngine(LuaEngine *engine) {
    // Outbound signals — Auto connections become queued automatically once
    // the engine lives on a worker thread (evaluated at emit time), so these
    // need no change beyond the engine's affinity.
    connect(engine, &LuaEngine::sendToTerminal, this, &PluginManager::sendToTerminal);
    connect(engine, &LuaEngine::showNotification, this, &PluginManager::showNotification);
    connect(engine, &LuaEngine::setStatusText, this, &PluginManager::statusMessage);
    connect(engine, &LuaEngine::logMessage, this, &PluginManager::logMessage);
    connect(engine, &LuaEngine::clipboardWriteRequested,
            this, &PluginManager::clipboardWriteRequested);
    // ANTS-1750 § 2.3 — settings.get is the one synchronous worker→GUI read.
    // BlockingQueuedConnection: the WORKER blocks until the GUI fills
    // QString &out (Config is touched only on the GUI/Config thread). The
    // GUI never blocks on a worker (§ 2.4), so the directions are acyclic —
    // no deadlock. Intentionally unbounded (§ 2.3): only the plugin waits,
    // never the UI. The second hop (PM → MainWindow lambda) stays a direct
    // same-thread call that runs inside this block, so `out` is filled before
    // the worker resumes.
    connect(engine, &LuaEngine::settingsGetRequested,
            this, &PluginManager::settingsGetRequested,
            Qt::BlockingQueuedConnection);
    // settings.set returns nothing into Lua — plain queued cross-thread.
    connect(engine, &LuaEngine::settingsSetRequested,
            this, &PluginManager::settingsSetRequested);
    connect(engine, &LuaEngine::paletteEntryRegistered,
            this, &PluginManager::paletteEntryRegistered);
    // ANTS-1750 — execution-time bracketing for healthTick(). These slots
    // run on the GUI thread (queued from the worker); m_execStart is touched
    // only here and in healthTick(). INV-3.
    connect(engine, &LuaEngine::eventStarted, this, [this, engine](quint64) {
        m_execStart.insert(engine, QDateTime::currentMSecsSinceEpoch());
    });
    connect(engine, &LuaEngine::eventCompleted, this, [this, engine](quint64) {
        m_execStart.remove(engine);
    });
}

void PluginManager::firePaletteAction(const QString &pluginName, const QString &actionId) {
    // ANTS-1750 INV-10 — targeted dispatch must not run lua_* on the GUI
    // thread; route through the queued dispatchTo().
    dispatchTo(pluginName, PluginEvent::PaletteAction, actionId);
}

void PluginManager::dispatchTo(const QString &pluginName, PluginEvent event,
                               const QString &data) {
    LuaEngine *engine = m_engines.value(pluginName, nullptr);
    if (!engine || m_demoted.contains(engine)) return;
    QMetaObject::invokeMethod(engine, "dispatchEvent", Qt::QueuedConnection,
                              Q_ARG(int, int(event)), Q_ARG(QString, data));
}

QList<LuaEngine *> PluginManager::healthyEngines() const {
    QList<LuaEngine *> out;
    out.reserve(m_engines.size());
    for (auto *engine : m_engines) {
        if (engine && !m_demoted.contains(engine)) out << engine;
    }
    return out;
}

void PluginManager::healthTick() {
    if (m_execStart.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Collect first so demotion doesn't mutate m_execStart mid-iteration.
    QList<LuaEngine *> toDemote;
    for (auto it = m_execStart.cbegin(); it != m_execStart.cend(); ++it) {
        LuaEngine *engine = it.key();
        if (!engine || m_demoted.contains(engine)) continue;
        // Execution time, not queue wait: a backed-up but healthy worker
        // re-arms eventStarted per event so each stays short; a worker stuck
        // in one C call emits eventStarted then never eventCompleted, so its
        // execution time ages past the deadline. INV-3.
        if (now - it.value() > engine->pcallBudgetMs() + kHealthGraceMs)
            toDemote << engine;
    }
    for (LuaEngine *engine : toDemote) {
        engine->requestAbort();   // raises luaL_error if the hook can fire
        m_demoted.insert(engine);
        emit logMessage(QString("Plugin demoted (handler exceeded %1ms): %2")
                            .arg(engine->pcallBudgetMs() + kHealthGraceMs)
                            .arg(engine->pluginName()));
    }
}

// 0.7.53 (2026-04-27 indie-review HIGH) — canonical permission allow-
// list. The manifest schema only recognises permissions the engine
// actually gates on (luaengine.cpp `hasPermission(...)` call sites).
// Anything else in the manifest's `permissions` array is silently
// dropped — the prior behaviour appended unknown strings into
// info.permissions, which fed the user-facing prompt with garbage
// names ("Plugin foo wants `delete_root`?") and let plugins claim
// privileges they couldn't actually exercise to phish for trust.
//
// Keep this list in sync with luaengine.cpp's hasPermission() call
// sites — every entry must correspond to a real gate on the Lua API
// surface, and every gate must have a matching entry here.
static const QStringList &knownPermissions() {
    static const QStringList kKnown = {
        QStringLiteral("clipboard.write"),
        QStringLiteral("settings"),
    };
    return kKnown;
}

static void parseManifestInto(PluginInfo &info, const QJsonObject &obj) {
    // Indie-review-2026-05-14 lane-6 C-1: do NOT let the manifest
    // override `info.name`. The on-disk directory name is the safe
    // primary key — it gates the grant store, the engine registry,
    // and the permission prompt. A spoofed manifest name would let an
    // attacker plugin inherit another plugin's grants. The optional
    // human-readable form lives in `displayName` instead.
    info.displayName = obj["name"].toString(info.name);
    info.description = obj["description"].toString();
    info.version = obj["version"].toString("0.1.0");
    info.author = obj["author"].toString();
    if (obj.contains("permissions") && obj["permissions"].isArray()) {
        QStringList perms;
        for (const auto &v : obj["permissions"].toArray()) {
            if (!v.isString()) continue;
            const QString p = v.toString();
            // Filter against the engine's known-permission allow-list.
            // Unknown strings are silently dropped (rather than passed
            // through to the user prompt) to prevent unexercisable
            // requests from inflating the plugin's apparent privilege
            // surface.
            if (knownPermissions().contains(p)) perms << p;
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

        // Reject a symlinked init.lua. The plugin directory is already
        // canonicalised, but a symlink at init.lua itself could point
        // at /etc/passwd, ~/.ssh/id_rsa, or any other file the user can
        // read; parsing fails harmlessly, but Lua's error string would
        // leak the file's first line via lua_tostring(err) when the
        // parse fails.
        if (QFileInfo(initLua).isSymLink()) {
            qWarning("Plugin %s: init.lua is a symlink — rejected",
                     qUtf8Printable(pluginDirName));
            continue;
        }

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
            // 0.7.53 (2026-04-27 indie-review HIGH) — intersect the
            // prompt result with the requested set. The user-facing
            // prompt CAN return more permissions than the manifest
            // asked for (e.g. a buggy implementation that returns
            // every checkbox on the dialog regardless of source);
            // intersection prevents a manifest from acquiring
            // privileges it never declared. Also, since the manifest
            // is filtered against knownPermissions() above, the
            // intersection here is automatically also filtered
            // against the canonical list — no path lets unknown
            // permission strings reach engine->setPermissions.
            QStringList raw = m_permissionPrompt(info, info.permissions);
            for (const QString &p : raw) {
                if (info.permissions.contains(p)) granted << p;
            }
            if (m_grantSave) m_grantSave(info.name, granted);
        } else {
            // Indie-review-2026-05-14 lane-6 M-2: filter the saved
            // grant list against the manifest's CURRENT permissions
            // before honouring it. Pre-fix, if a plugin shrunk its
            // requested permissions between sessions (manifest edited
            // from ["clipboard.write", "settings"] to just
            // ["clipboard.write"]), the engine still ran with both
            // grants because `saved` still contained the old one.
            // The hasPermission gates each ants.* call, but the
            // manifest-vs-engine asymmetry violates the documented
            // contract that the manifest is the source of truth.
            for (const QString &p : saved) {
                if (info.permissions.contains(p)) granted << p;
            }
        }
    }

    // ANTS-1750 — parentless engine (Qt refuses moveToThread on a parented
    // QObject) moved onto its own worker QThread. Lifetime is managed
    // explicitly: teardownEngine() deletes it worker-side on a clean unload,
    // or detaches it into m_zombies if its worker is wedged. INV-11.
    auto *engine = new LuaEngine(nullptr);
    engine->setPluginName(info.name);
    engine->setPermissions(granted);
    wireEngine(engine);  // connect signals while the engine is still on the GUI thread

    // Parentless thread too — a zombie (stuck) thread must outlive PM, so it
    // can't be a PM child (deleting a running QThread aborts). teardownEngine
    // owns the clean-path delete; zombies leak until process exit by design.
    auto *thread = new QThread(nullptr);
    thread->setObjectName(QStringLiteral("ants-plugin-") + info.name);
    // INV-9 — explicit small worker stack. The sandbox can't deep-recurse
    // (Lua bounds C-call depth at LUAI_MAXCCALLS = 200 and the ants.* shims
    // are shallow), so 512 KiB is safe with comfortable headroom.
    thread->setStackSize(512 * 1024);
    engine->moveToThread(thread);
    thread->start();

    m_engines.insert(info.name, engine);
    m_threads.insert(info.name, thread);

    QString initLua = info.path + "/init.lua";
    if (devMode()) {
        emit logMessage(QString("[plugin-dev] loading %1 from %2 (granted: %3)")
                        .arg(info.name, initLua, granted.join(',')));
    }
    // Three queued posts to the same worker, processed FIFO:
    // initialize → loadScript(init.lua) → Load. Failure gating is worker-side
    // via the m_state latch (§ 2.2.2): a failed initialize leaves m_state
    // null so the following posts no-op; a loadScript script error calls
    // shutdown() on the worker, nulling m_state and gating Load. The GUI
    // never waits on any of the three.
    QMetaObject::invokeMethod(engine, "initialize", Qt::QueuedConnection);
    QMetaObject::invokeMethod(engine, "loadScript", Qt::QueuedConnection,
                              Q_ARG(QString, initLua));
    QMetaObject::invokeMethod(engine, "dispatchEvent", Qt::QueuedConnection,
                              Q_ARG(int, int(PluginEvent::Load)),
                              Q_ARG(QString, info.name));
    emit logMessage(QString("Loading plugin: %1 v%2").arg(info.name, info.version));
}

bool PluginManager::fireEvent(PluginEvent event, const QString &data) {
    // ANTS-1750 — non-blocking broadcast: post to each healthy worker and
    // return to the GUI event loop immediately (INV-2). No veto for async
    // events — every event fired today is fire-and-forget; the synchronous-
    // veto path is ANTS-1736 (§ 2.6).
    for (auto *engine : healthyEngines()) {
        QMetaObject::invokeMethod(engine, "dispatchEvent", Qt::QueuedConnection,
                                  Q_ARG(int, int(event)), Q_ARG(QString, data));
    }
    return true;
}

void PluginManager::setRecentOutput(const QString &output) {
    // ANTS-2001 — NB: no caller wires this yet, so ants.get_output() returns
    // empty in shipping builds (PLUGINS.md documents the gap). Reserved
    // producer for a future output-feed commit.
    // ANTS-1750 INV-6 — queued so m_recentOutput is mutated on the worker.
    for (auto *engine : healthyEngines())
        QMetaObject::invokeMethod(engine, "setRecentOutput", Qt::QueuedConnection,
                                  Q_ARG(QString, output));
}

void PluginManager::setCwd(const QString &cwd) {
    // ANTS-2001 — NB: no caller wires this yet, so ants.get_cwd() returns
    // empty in shipping builds (PLUGINS.md documents the gap). Reserved
    // producer for a future cwd-feed commit.
    // ANTS-1750 INV-6 — queued so m_cwd is mutated on the worker.
    for (auto *engine : healthyEngines())
        QMetaObject::invokeMethod(engine, "setCwd", Qt::QueuedConnection,
                                  Q_ARG(QString, cwd));
}
