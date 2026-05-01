#pragma once

#include "config.h"
#include "claudeintegration.h"  // for ClaudeState — used by applyClaudeStatusLabel()
#include "elidedlabel.h"        // ElidedLabel — status-bar text slots
#include "themes.h"

#include <QMainWindow>
#include <QTabWidget>
#include <QActionGroup>
#include <QLabel>
#include <QTimer>
#include <QProgressBar>
#include <QPushButton>
#include <QPropertyAnimation>
#include <QFileSystemWatcher>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QShortcut>
#include <QHash>

class QNetworkAccessManager;
class TitleBar;
class OpaqueMenuBar;
class OpaqueStatusBar;
class TerminalWidget;
class CommandPalette;
class AiDialog;
class SshDialog;
class SettingsDialog;
class ClaudeAllowlistDialog;
class ClaudeProjectsDialog;
class ClaudeTranscriptDialog;
class ClaudeIntegration;
class ClaudeTabTracker;
class ClaudeBgTaskTracker;
class ClaudeStatusBarController;
class ColoredTabBar;
class ColoredTabWidget;
class QSplitter;
class QFrame;
class KWinPositionTracker;
class GlobalShortcutsPortal;
class RemoteControl;
#ifdef ANTS_LUA_PLUGINS
class PluginManager;
#endif

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(bool quakeMode = false, QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onTitleChanged(const QString &title);
    void toggleMaximize();

    void newTab();
    void closeTab(int index);
    void closeCurrentTab();

    // ANTS-1102: confirm-on-close helpers. closeTab() does the
    // descendant-process probe and routes through showCloseTabConfirmDialog
    // when a non-shell descendant is found; both confirm and silent
    // paths land in performTabClose for the actual teardown.
    void performTabClose(int index);
    void showCloseTabConfirmDialog(QWidget *tabWidget,
                                   const QString &processName);
    void onTabChanged(int index);

    // Split pane
    void splitHorizontal();
    void splitVertical();
    void closeFocusedPane();

    // SSH connection
    void onSshConnect(const QString &sshCommand, bool newTab);

private:
    void setupMenus();
    void applyTheme(const QString &name);
    void centerWindow();
    void moveViaKWin(int targetX, int targetY);
    void changeFontSize(int delta);
    void applyFontSizeToAll(int size);
public:
    // Remote-control data accessors — public because RemoteControl is
    // a sibling class (not a friend) and each contract below is a
    // narrow, intentional seam for rc_protocol commands. Every new
    // rc_protocol command adds one accessor here so the lookup /
    // dispatch split stays clean.
    //
    // `currentTerminal()` — the active TerminalWidget (focused pane
    // in split layouts, else the first pane). Used as the default
    // target for commands that don't specify a tab.
    TerminalWidget *currentTerminal() const;
    TerminalWidget *focusedTerminal() const;

    // `tabListForRemote()` returns one JSON object per tab with
    // `index`, `title`, `cwd`, `active` (used by the `ls` command).
    QJsonArray tabListForRemote() const;

    // `tabsAsJson()` is the richer surface used by the `tab-list` IPC
    // verb (ANTS-1117). Returns one JSON object per tab with the same
    // `index`/`title`/`cwd` plus `shell_pid` (for process scripting),
    // `claude_running` (true iff `ClaudeTabTracker::shellState(pid)
    // .state != ClaudeState::NotRunning`), and `color` (the per-tab
    // colored-tab-bar override colour, or empty when default). The
    // `ls` verb's narrow shape stays unchanged for backward compat.
    QJsonArray tabsAsJson() const;

    // `roadmapPathForRemote()` exposes `m_roadmapPath` (the cached
    // ROADMAP.md path probed by `refreshRoadmapButton`) to the
    // `roadmap-query` IPC verb. Returns empty when no ROADMAP.md was
    // detected for the active tab's CWD. Const-ref return — member
    // outlives the call (ANTS-1122 audit-fold-in 2026-04-30).
    const QString &roadmapPathForRemote() const;

    // `terminalAtTab(i)` returns the "active" terminal inside the
    // tab at index `i` (for split layouts that's the focused pane,
    // else the first). Returns nullptr on out-of-range index or
    // when no terminal is attached. Used by the `send-text` command;
    // subsequent commands (`get-text`, `select-window`, etc.) will
    // share this accessor.
    TerminalWidget *terminalAtTab(int index) const;

    // `selectTabForRemote(i)` switches the active tab to `i`. Returns
    // false if the index is out of range — caller propagates as an
    // error envelope to the rc_protocol client. Focuses the new tab's
    // terminal after switching so subsequent `send-text` calls without
    // an explicit `tab` field hit the expected pane.
    bool selectTabForRemote(int index);

    // `currentTabIndexForRemote()` returns the active tab's 0-based
    // index, or -1 when there are no tabs (theoretical — MainWindow
    // always keeps ≥ 1). Used by rc_protocol commands that take an
    // optional `tab` field and need to resolve "active" to an index.
    int currentTabIndexForRemote() const;

    // `setTabTitleForRemote(i, title)` sets the tab label and pins it
    // — the pin survives both the per-shell `titleChanged` signal
    // (OSC 0/2 from the inferior) and the 2 s `updateTabTitles`
    // refresh. Empty `title` removes the pin and lets the auto-title
    // mechanism take over again. Returns false on out-of-range index.
    bool setTabTitleForRemote(int index, const QString &title);

    // `newTabForRemote` opens a new tab and returns its index. Used
    // by the `new-tab` rc_protocol command.
    //
    //   `cwd`     — empty string → inherit from the focused / current
    //               terminal (same default as the menu-driven newTab()
    //               slot). Non-empty → start the tab's shell in that
    //               directory.
    //   `command` — empty → no-op after shell start. Non-empty → write
    //               the string as a command into the new shell after
    //               a 200 ms settle (same pattern as onSshConnect
    //               uses for shell wiring). Callers are responsible
    //               for line terminators in `command`; the shell
    //               won't execute until a newline lands.
    int newTabForRemote(const QString &cwd, const QString &command);

private:

    // Split helpers
    TerminalWidget *createTerminal();
    void connectTerminal(TerminalWidget *terminal);
    void splitCurrentPane(Qt::Orientation orientation);
    QSplitter *findParentSplitter(QWidget *w) const;
    void cleanupEmptySplitters(QWidget *tabRoot);

    // Apply settings from config to a terminal
    void applyConfigToTerminal(TerminalWidget *terminal);

    // Session persistence
    void saveAllSessions();
    void restoreSessions();

    TitleBar *m_titleBar = nullptr;
    OpaqueMenuBar *m_menuBar = nullptr;
    OpaqueStatusBar *m_statusBar = nullptr;
    ColoredTabWidget *m_tabWidget = nullptr;
    ColoredTabBar *m_coloredTabBar = nullptr;  // = m_tabWidget->coloredTabBar()
    Config m_config;
    QString m_currentTheme;

    QActionGroup *m_themeGroup = nullptr;
    QActionGroup *m_scrollbackGroup = nullptr;
    QActionGroup *m_opacityGroup = nullptr;

    CommandPalette *m_commandPalette = nullptr;
    void collectActions(QMenu *menu, QList<QAction *> &out);

    // AI assistant
    AiDialog *m_aiDialog = nullptr;

    // SSH manager
    SshDialog *m_sshDialog = nullptr;

    // Settings dialog
    SettingsDialog *m_settingsDialog = nullptr;

    // Broadcast mode
    bool m_broadcastMode = false;
    QAction *m_broadcastAction = nullptr;

    // Status bar widgets. Layout principle (from user feedback 2026-04-18):
    // only the transient notification slot (m_statusMessage) is elastic and
    // elides with "…"; every other widget on the bar is **fixed** — it
    // reports its natural sizeHint with a horizontally-Fixed size policy
    // and must never be squeezed to cater for the notification. That
    // guarantees the git branch name, foreground-process name, Claude
    // status, and both transient buttons always display in full. Past
    // elide-to-"…" regressions on the branch label were all traced back
    // to ElidedLabel under QBoxLayout pressure; using plain QLabel with
    // Fixed horizontal policy makes the widget unsqueezable.
    QLabel *m_statusGitBranch = nullptr;
    QFrame *m_statusGitSep = nullptr;     // vertical divider after branch chip
    ElidedLabel *m_statusMessage = nullptr;
    QLabel *m_statusProcess = nullptr;
    QTimer *m_statusTimer = nullptr;
    QTimer *m_statusMessageTimer = nullptr;
    void updateStatusBar();
    void updateTabTitles();
    // Git branch cache — avoid synchronous .git/HEAD tree walk on every poll
    QString m_gitCacheCwd;
    QString m_gitCacheBranch;
    qint64 m_gitCacheMs = 0;
    // Route status messages to m_statusMessage instead of statusBar()->showMessage(),
    // which would hide the git branch and cwd labels while displayed.
    // Timeout semantics:
    //   timeoutMs  < 0 (default): use config.notificationTimeoutMs()
    //                             (user-configurable, default 5000 ms).
    //   timeoutMs == 0          : pin the message — never auto-clear.
    //                             Used for the Claude permission prompt
    //                             which must stay visible until the user
    //                             approves/declines.
    //   timeoutMs  > 0          : clear after that many milliseconds.
    void showStatusMessage(const QString &msg, int timeoutMs = -1);
    void clearStatusMessage();

    // Quake mode
    bool m_quakeMode = false;
    bool m_quakeVisible = false;
    QPropertyAnimation *m_quakeAnim = nullptr;
    // 0.6.39: Freedesktop Portal GlobalShortcuts client (owned). Nullptr
    // when the portal service isn't registered (no xdg-desktop-portal)
    // OR when the bind request was rejected (e.g. GNOME Shell, which
    // hasn't implemented the GlobalShortcuts portal yet). In both cases
    // the in-app QShortcut from 0.6.38 stays as the activation path.
    GlobalShortcutsPortal *m_gsPortal = nullptr;
    // Monotonic-ms timestamp of the last Quake toggle, keyed by which
    // path fired. Used to debounce the portal-vs-QShortcut double-fire
    // when Ants is focused and both paths route the same key press.
    qint64 m_lastQuakeToggleMs = 0;
    void setupQuakeMode();
    void toggleQuakeVisibility();

    // Undo close tab
    struct ClosedTabInfo {
        QString cwd;
        QString title;
    };
    QList<ClosedTabInfo> m_closedTabs; // max 10

    // Tab color groups — storage lives in m_coloredTabBar (QTabBar::tabData
    // per tab); MainWindow only exposes the context-menu entry point.
    void showTabColorMenu(int tabIndex);

    // Persist per-tab colour choices across restarts. Storage lives in the
    // `tab_groups` config key as a JSON object mapping the tab's UUID
    // (m_tabSessionIds value) to an "#rrggbbaa" hex string. Keyed by UUID
    // rather than index so drag-reorder doesn't invalidate entries, and
    // rather than tab title so rename doesn't silently orphan them.
    // Persistence is unconditional (not gated on session_persistence) —
    // user requested colours survive even with session restore disabled.
    void persistTabColor(QWidget *tabRoot, const QColor &color);
    void applyPersistedTabColor(QWidget *tabRoot);
    // Persist the full current tab color list (by index) to
    // config.tab_color_sequence. Called after any tab-color mutation
    // and on app close. Independent of session persistence — the
    // ordered list survives restart regardless.
    void saveTabColorSequence();
    // Apply the ordered color list from config.tab_color_sequence to
    // tabs by index. Called after the initial tab set is created
    // (either via restoreSessions or the single default newTab), as a
    // fallback for when UUID-keyed persistTabColor couldn't match
    // (session persistence off → UUIDs regenerated on restart).
    void applyTabColorSequence();

    // Handle trigger signals from terminals
    void onTriggerFired(const QString &pattern, const QString &actionType, const QString &actionValue);

    // 0.6.9 — palette entries registered by plugins via ants.palette.register({...}).
    // Maintained as a parallel list to the menu-derived QActions; whenever this
    // changes (plugin load/reload/unload, new register call) we rebuild the
    // CommandPalette's full action list. Hotkey strings, when non-empty, get
    // QShortcut wrappers that route through PluginManager::firePaletteAction.
#ifdef ANTS_LUA_PLUGINS
    struct PluginPaletteEntry {
        QString plugin;     // owning plugin name
        QString title;      // visible label
        QString action;     // payload echoed back to plugin
        QString hotkey;     // empty if no shortcut
        QAction *qaction = nullptr;     // owned by `this`
        QShortcut *shortcut = nullptr;  // owned by `this`, optional
    };
    QList<PluginPaletteEntry> m_pluginPaletteEntries;
    void rebuildCommandPalette();
    void onPluginPaletteRegistered(const QString &pluginName, const QString &title,
                                    const QString &action, const QString &hotkey);
    void clearPluginPaletteEntriesFor(const QString &pluginName);
#endif

    // Claude Code integration
    ClaudeIntegration *m_claudeIntegration = nullptr;
    // Per-tab state tracker for the tab-bar activity glyph. Separate
    // from m_claudeIntegration (which tracks one active tab at a time
    // for the bottom status bar) — this maintains state for every tab
    // with a Claude child simultaneously. Null when the config toggle
    // claude_tab_status_indicator is off. See
    // tests/features/claude_tab_status_indicator/spec.md.
    ClaudeTabTracker *m_claudeTabTracker = nullptr;
    ClaudeAllowlistDialog *m_claudeDialog = nullptr;
    ClaudeProjectsDialog *m_claudeProjects = nullptr;
    ClaudeTranscriptDialog *m_claudeTranscript = nullptr;
    // ANTS-1146 — Claude status-bar widgets + per-session render
    // state + the permission-button-group factory live on the
    // controller; MainWindow holds it but doesn't reach inside.
    // Legacy refresh paths use accessors (reviewButton(),
    // bgTasksButton()); manual state pokes use setters
    // (setPromptActive, setPlanMode, setAuditing, setError,
    // resetForTabSwitch). Theme changes route through applyTheme.
    ClaudeStatusBarController *m_claudeStatusBarController = nullptr;
    // ANTS-1013 indie-review-2026-04-27: in-flight de-dup for the
    // 2 s `git status --porcelain=v1 -b` probe. The status timer used
    // to spawn a fresh QProcess every tick even while the previous
    // probe was still alive — visible cost on slow filesystems or
    // pathologically large repos (the user's repo took ~600ms on a
    // cold cache once). Guard skips the spawn while a probe is in
    // flight; cleared by both the finished and errorOccurred handlers.
    bool m_reviewProbeInFlight = false;
    // 0.7.39 — status-bar Roadmap viewer button. Visible iff the
    // active tab's cwd contains a ROADMAP.md (case-insensitive).
    // Click → RoadmapDialog (live-watching, filterable, current-work
    // highlight from CHANGELOG [Unreleased] + recent commits).
    QPushButton *m_roadmapBtn = nullptr;
    QString m_roadmapPath;  // last canonical path the button was wired against
    // 0.7.45 — Repo visibility badge. Small Public/Private indicator
    // for the active tab's GitHub repo. Hidden when the cwd isn't a
    // GitHub repo or `gh` is unavailable.
    QLabel *m_repoVisibilityLabel = nullptr;
    struct RepoVisibilityCache {
        QString visibility;  // "PUBLIC" / "PRIVATE" / "" (negative cache)
        qint64 fetchedAt = 0;  // ms since epoch
    };
    QHash<QString /*repoRoot*/, RepoVisibilityCache> m_repoVisibilityCache;
    // ANTS-1137 — in-flight guard mirroring m_reviewProbeInFlight.
    // Drops redundant `gh repo view` probes when a fast tab-switch
    // would otherwise race two QProcesses against the same repoRoot.
    QHash<QString /*repoRoot*/, bool> m_repoVisibilityProbeInFlight;
    bool m_ghAvailable = false;        // probed once at startup
    bool m_ghAvailableProbed = false;  // probe-once flag
    // 0.7.62 (ANTS-1124) — Update-available notifier moved from a
    // status-bar QLabel to a top-level menu-bar QAction. Sits to
    // the right of &Help by call order; surfaced when the
    // version-check probe finds a newer release. URL is stored on
    // the action via setData; the triggered slot replays it
    // through handleUpdateClicked().
    QAction *m_updateAvailableAction = nullptr;
    QString m_latestRemoteVersion;  // last seen tag_name from GitHub
    QNetworkAccessManager *m_updateNam = nullptr;
    // ANTS-1146 — m_claudeLastState / LastDetail / PromptActive /
    // PlanMode / Auditing all moved onto ClaudeStatusBarController.
    // MainWindow's terminal-event handlers poke them via the
    // controller's setPromptActive / setPlanMode / setAuditing
    // setters; the auto-state ClaudeIntegration::stateChanged /
    // planModeChanged / auditingChanged signals are consumed by
    // the controller's own connect blocks set up in attach().
    // Single entry point that refreshes every per-tab status-bar widget
    // (branch chip, notification, process label, Claude state, Review
    // Changes button, Add-to-allowlist button) against the currently
    // active tab's terminal. Called from onTabChanged plus after any
    // async probe completes. Replaces the scatter of direct
    // updateStatusBar / refreshReviewButton / applyClaudeStatusLabel
    // calls that accumulated in onTabChanged and let per-tab state
    // bleed from tab A to tab B.
    void refreshStatusBarForActiveTab();
    void openClaudeAllowlistDialog(const QString &prefillRule = QString());
    void openClaudeProjectsDialog();
    // ANTS-1146 — formerly setupClaudeIntegration. Constructs
    // ClaudeIntegration, the controller, the MCP-provider helper,
    // and the three orphan chrome items (Roadmap button,
    // update QAction, 5 s startup update-check).
    void setupStatusBarChrome();
    // ANTS-1146 — MCP-provider plumbing for ClaudeIntegration
    // (scrollback / cwd / lastCommand / git-status / environment
    // providers + startHookServer). Split from setupClaudeIntegration
    // because it's not status-bar chrome.
    void setupClaudeMcpProviders();
    void showDiffViewer();
    // Re-check git diff state and enable/disable the Review Changes
    // button accordingly. Async (QProcess) so it never blocks the UI
    // thread. Called on fileChanged events, tab switches, and after
    // the diff viewer finds no changes (to avoid clicking a button
    // that then shows "no changes" again).
    void refreshReviewButton();
    void showBgTasksDialog();
    // ANTS-1146 — refreshBgTasksButton moved to
    // ClaudeStatusBarController. Three call sites in mainwindow.cpp
    // (status timer connect, onTabChanged, showBgTasksDialog
    // post-dismissal) call m_claudeStatusBarController->
    // refreshBgTasksButton() directly.
    // Probe the active tab's cwd for a ROADMAP.md (case-insensitive)
    // and show/hide the Roadmap button. Called from the central
    // refreshStatusBarForActiveTab tick.
    void refreshRoadmapButton();
    // Open the Roadmap dialog against m_roadmapPath. Non-modal so the
    // user can keep typing in the terminal while reading.
    void showRoadmapDialog();
    // 0.7.45 — Probe the active tab's cwd for a `.git` ancestor,
    // parse `[remote "origin"]` from `.git/config` to extract
    // owner/repo, and show a "Public" / "Private" badge resolved via
    // `gh repo view`. Result cached by repoRoot for 10 minutes;
    // failures (no .git, non-GitHub origin, gh missing, gh
    // unauthenticated, network) all hide the label.
    void refreshRepoVisibility();
    // 0.7.45 — Hit GitHub's `releases/latest` endpoint via QNAM,
    // compare `tag_name` against ANTS_VERSION, and surface a
    // clickable status-bar label when newer. No-op silently on
    // network failure or rate-limit. `userInitiated` (added 0.7.47)
    // enables status-bar feedback for the "no update available" /
    // "network error" cases — startup probes stay silent so the
    // launch path doesn't surface negative results that the user
    // didn't ask for.
    void checkForUpdates(bool userInitiated = false);
    // 0.7.46 — Click handler for the update-available label. Probes
    // for AppImageUpdate (GUI) or appimageupdatetool (CLI); if found
    // AND $APPIMAGE is set (binary is running as an AppImage),
    // launches the updater detached. Falls back to opening the
    // release page in the browser when neither is installed or the
    // binary isn't an AppImage. The `url` argument is the release
    // page URL emitted by the linkActivated signal.
    void handleUpdateClicked(const QString &url);

    // Plugin manager
#ifdef ANTS_LUA_PLUGINS
    PluginManager *m_pluginManager = nullptr;
    QList<QShortcut *> m_pluginShortcuts; // shortcuts registered via manifest "keybindings"
#endif

    // Tab UUIDs for session persistence
    QHash<QWidget *, QString> m_tabSessionIds;

    // Per-tab manual title pins (set by rc_protocol `set-title`).
    // When a tab's QWidget* is in this map, the per-shell
    // `titleChanged` handler and the 2 s `updateTabTitles` refresh
    // both leave that tab's label alone — the pinned value sticks
    // until rc_protocol clears it (empty `title` → remove from map).
    // Cleaned up alongside `m_tabSessionIds` at tab destruction.
    QHash<QWidget *, QString> m_tabTitlePins;

    // First-show flag (per-instance, not static)
    bool m_firstShow = true;

    // XCB position tracker (Qt's pos()/moveEvent broken for frameless windows on KWin)
    KWinPositionTracker *m_posTracker = nullptr;

    // Hot-reload: watch config.json for external changes
    QFileSystemWatcher *m_configWatcher = nullptr;
    void onConfigFileChanged(const QString &path);
    // Re-entrancy guard for onConfigFileChanged. Set true on entry, cleared
    // by a deferred singleShot that fires after Qt's event loop has had a
    // chance to drain any inotify events queued by save() calls inside the
    // reload path. Without this, save()-from-applyTheme triggers a fresh
    // fileChanged after blockSignals(false), and the slot re-enters in a
    // loop. Prior 0.7.31 fix (blockSignals on the watcher) is ineffective
    // because the inotify event is dispatched after onConfigFileChanged
    // returns. See tests/features/config_reload_loop_safety/spec.md.
    bool m_inConfigReload = false;

    // Dark/light mode auto-switching
    void onSystemColorSchemeChanged();

    // Auto-profile switching
    void checkAutoProfileRules(TerminalWidget *terminal);
    QString m_lastAutoProfile;

    // Command snippets
    void showSnippetsDialog();

    // Uptime — skip session save if app ran < 5s (test/crash scenario)
    QElapsedTimer m_uptimeTimer;

    // Main-thread stall detector — a heartbeat QTimer that measures
    // the gap between consecutive firings. When the event loop is
    // blocked (expensive handler, synchronous I/O, Lua plugin
    // hanging, etc.) the actual gap exceeds the scheduled interval
    // by the blockage duration. Gated by ANTS_DEBUG=perf so the
    // hot path is a single bit-test when disabled.
    QTimer *m_stallTimer = nullptr;
    QElapsedTimer m_stallLastFire;
    qint64 m_stallWorstMs = 0;
    quint64 m_stallCount = 0;

    // Remote-control JSON-over-Unix-socket server. Non-owning
    // pointer — MainWindow owns it via QObject parent/child tree.
    RemoteControl *m_remoteControl = nullptr;
};
