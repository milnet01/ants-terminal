#pragma once

#include <QString>
#include <QStringList>
#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QElapsedTimer>
#include <atomic>
#include <vector>

// Forward declare Lua state + activation record
struct lua_State;
struct lua_Debug;

// Event types for plugin hooks
enum class PluginEvent {
    Output,       // Terminal received output data
    Line,         // Complete line received
    Prompt,       // OSC 133 prompt detected
    KeyPress,     // Key pressed (before sending to PTY) — NOT dispatched yet
                  // (ANTS-1736 dormant; PLUGINS.md marks "keypress" †)
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

    bool isInitialized() const { return m_state != nullptr; }

    // Memory limit for Lua allocations (default 10MB per-VM)
    static constexpr size_t MAX_LUA_MEMORY = 10 * 1024 * 1024;

    // Fire events to all registered handlers. Returns false if any handler
    // requests cancellation (the dormant keypress veto — ANTS-1736). Runs
    // the lua_pcall loop on the owning thread; dispatchEvent() is the queued
    // worker-side wrapper that brackets it with eventStarted/eventCompleted
    // for the controller's health check.
    bool fireEvent(PluginEvent event, const QString &data = QString());

    // ANTS-1750 — the GUI controller (PluginManager) sets the abort flag
    // when it demotes a runaway plugin; instructionHook reads it and raises
    // luaL_error at the next VM boundary. Atomic: the only LuaEngine state
    // intentionally touched from a thread other than the worker. INV-5.
    void requestAbort() { m_abortRequested.store(true); }
    // Per-pcall wall-clock budget (ms). Read by the controller's health tick
    // to derive the execution-time deadline (budget + grace). Set-once,
    // before the engine is threaded.
    qint64 pcallBudgetMs() const { return m_pcallBudgetMs; }

    // Per-pcall wall-clock budget (default 1500 ms). Test-only knob —
    // production callers leave it at the default so a normal plugin
    // gets the full budget; timeout-enforcement tests drop it to a
    // small value to assert the kill path without sleeping ~1.5 s
    // per pcall on every CI run. Applies to the next call to
    // `startPcallBudget()` (i.e. the next outer pcall launched by
    // `loadScript` or an event firing).
    void setPcallBudgetMs(qint64 ms) { m_pcallBudgetMs = ms; }

    // ===================================================================
    // ANTS-2093 — project_query: run an agent-supplied READ-ONLY Lua
    // snippet server-side and return only its marshalled result. Reuses
    // the plugin sandbox (5-lib allowlist + sandboxEnvironment() +
    // allocator + hook) but installs ONLY the read-only `project.*` API
    // (registerQueryApi) — none of registerApi()'s write callbacks.
    // See docs/specs/ANTS-2093.md.
    // ===================================================================

    // Marshalled outcome of one snippet run. Invariant: code/error are
    // non-empty iff ok==false; a success carries neither, a refusal both.
    struct QueryResult {
        bool       ok = false;
        QJsonValue result;     // §2.4 marshal of the snippet's return value
        QString    code;       // refusal code — set iff !ok (INV-8)
        QString    error;      // operator-facing message — set iff !ok
        qint64     elapsedMs = 0;  // wall-clock around the lua_pcall
    };

    // Install ONLY the read-only `project` table (read/list/root, §2.2)
    // for query mode. `root` is the canonicalised caller_cwd that scopes
    // every project.* file access (§2.3, FS confinement via validatePath).
    void registerQueryApi(const QString &root);

    // Run `code` text-only under the budget on THIS fresh VM configured
    // for `root`, marshalling the single top-of-stack return value under
    // `resultCapBytes`. Synchronous; must run on the engine's own thread.
    // Pure outcome — no signals emitted in query mode (§2.4).
    QueryResult runQuery(const QString &code, const QString &root,
                         qint64 timeoutMs, int resultCapBytes);

    // ANTS-2093 §2.4 — orchestrate runQuery() on a fresh ephemeral worker
    // thread with a BOUNDED join (timeout + 250 ms grace). A snippet stuck
    // in an uninterruptible C call (which the hook can't preempt) is
    // DETACHED at the deadline — the call returns query_timeout and the
    // caller's thread resumes; the worker is held as a zombie until process
    // exit (its lua_State is owned by the stuck C frame). Safe to call from
    // the GUI/dispatch thread.
    static QueryResult runQueryThreaded(const QString &code,
                                        const QString &root,
                                        qint64 timeoutMs, int resultCapBytes);

    // ANTS-2093 §2.5 — full `project_query` verb body: feature-gate FIRST
    // (off → query_disabled, before any arg validation), then caller_cwd /
    // code validation, then runQueryThreaded, then the success/refusal
    // envelope. Pure — no MainWindow/RemoteControl needed (so it lives in
    // ants_lua_lib where LuaEngine + PathValidation are both visible; the
    // core-lib RemoteControl cannot see LuaEngine).
    static QJsonObject projectQueryVerb(const QJsonObject &req, bool enabled,
                                        qint64 timeoutMs, int resultCapBytes);

    // §2.4 join grace added to the timeout before the worker is detached.
    static constexpr int kQueryJoinGraceMs = 250;

public slots:
    // Worker-side entry points (ANTS-1750). After the engine is moved onto
    // its worker QThread, PluginManager invokes these across the thread
    // boundary with Qt::QueuedConnection so the GUI thread never blocks on a
    // plugin. All are also safe to call directly on the owning thread (the
    // non-threaded unit tests do exactly that).
    bool initialize();
    bool loadScript(const QString &path);
    void shutdown();
    // Run the handlers for `event` (a PluginEvent cast to int for the
    // queued-call metatype) on the worker. Brackets the run with
    // eventStarted/eventCompleted so the controller can measure handler
    // execution time. Async — any veto return is ignored (§ 2.6).
    void dispatchEvent(int event, const QString &data);
    // Context setters — queued so m_recentOutput / m_cwd are mutated on the
    // worker thread only (no GUI-write / worker-read race). INV-6.
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
    // ANTS-1750 — execution-time bracketing for the single-uninterruptible-
    // C-call health check. Emitted by dispatchEvent at the top (before the
    // lua_pcall loop) and after it; PluginManager records the execution
    // start time from eventStarted and clears it on eventCompleted, so the
    // health tick measures handler execution time, not queue wait.
    void eventStarted(quint64 seq);
    void eventCompleted(quint64 seq);

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
    static int lua_ants_warn(lua_State *L);   // ANTS-2001 — warn → ants.log
    static int lua_ants_clipboard_write(lua_State *L);
    static int lua_ants_settings_get(lua_State *L);
    static int lua_ants_settings_set(lua_State *L);
    static int lua_ants_palette_register(lua_State *L);

    // ANTS-2093 — read-only query API callbacks (project.*). Each anchors
    // its path argument to m_queryRoot via PathValidation::validatePath
    // (§2.3) and raises a Lua error on escape / missing file.
    static int lua_project_read(lua_State *L);
    static int lua_project_list(lua_State *L);
    static int lua_project_root(lua_State *L);

    void registerApi();
    void sandboxEnvironment();

    // ANTS-2093 — shared VM construction extracted from initialize() so the
    // query path reuses the EXACT sandbox setup (allowlist + allocator +
    // hook) and the two can't drift on a security fix. queryMode picks
    // registerQueryApi(queryRoot) over registerApi().
    bool buildVm(bool queryMode, const QString &queryRoot);

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
    // ANTS-2205 — monotonic budget timer (was an epoch-ms deadline). The hook
    // reads it per instruction-batch; QElapsedTimer uses CLOCK_MONOTONIC, so it
    // is cheaper than QDateTime::currentMSecsSinceEpoch() and immune to wall-
    // clock steps (NTP / manual set) that could otherwise fire or defer the
    // budget kill spuriously. Started in startPcallBudget(); !isValid() == unarmed.
    QElapsedTimer m_pcallTimer;
    qint64 m_pcallBudgetMs   = 1500;  // Tunable via setPcallBudgetMs().
    QString m_recentOutput;
    QString m_cwd;
    // ANTS-2093 — canonical caller_cwd that scopes project.* reads in query
    // mode. Set by registerQueryApi(); empty for a normal plugin VM.
    QString m_queryRoot;

    // ANTS-1750 — GUI-set abort flag (see requestAbort()). Read in
    // instructionHook; never cleared — a demoted plugin is neutered for the
    // rest of the session and receives no further events.
    std::atomic<bool> m_abortRequested{false};
    // ANTS-1750 — monotonic per-engine event sequence, worker-only. Tags
    // each eventStarted/eventCompleted pair (FIFO single-worker, so they
    // strictly alternate; the seq is for robustness + logging).
    quint64 m_seq = 0;

    // Event handlers: event -> list of Lua registry keys (function refs)
    QHash<PluginEvent, std::vector<int>> m_handlers;
};
