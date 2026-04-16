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
#include <QShortcut>

class TitleBar;
class TerminalWidget;
class CommandPalette;
class AiDialog;
class SshDialog;
class SettingsDialog;
class ClaudeAllowlistDialog;
class ClaudeProjectsDialog;
class ClaudeTranscriptDialog;
class ClaudeIntegration;
class ColoredTabBar;
class ColoredTabWidget;
class QSplitter;
class QFrame;
class XcbPositionTracker;
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

private slots:
    void onTitleChanged(const QString &title);
    void toggleMaximize();

    void newTab();
    void closeTab(int index);
    void closeCurrentTab();
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
    TerminalWidget *currentTerminal() const;
    TerminalWidget *focusedTerminal() const;

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
    QMenuBar *m_menuBar = nullptr;
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

    // Status bar widgets. The three text slots are ElidedLabels so they
    // truncate with "…" to a bounded width instead of growing unbounded
    // and pushing siblings off-screen — e.g. a long foreground-process
    // name (kernel TASK_COMM_LEN caps at 15 but kept defensive here) or
    // a long transient message ("Claude permission: Bash(git log…)").
    ElidedLabel *m_statusGitBranch = nullptr;
    QFrame *m_statusGitSep = nullptr;     // vertical divider after branch chip
    ElidedLabel *m_statusMessage = nullptr;
    ElidedLabel *m_statusProcess = nullptr;
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
    void showStatusMessage(const QString &msg, int timeoutMs = 0);
    void clearStatusMessage();

    // Quake mode
    bool m_quakeMode = false;
    bool m_quakeVisible = false;
    QPropertyAnimation *m_quakeAnim = nullptr;
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
    ClaudeAllowlistDialog *m_claudeDialog = nullptr;
    ClaudeProjectsDialog *m_claudeProjects = nullptr;
    ClaudeTranscriptDialog *m_claudeTranscript = nullptr;
    // m_claudeStatusLabel shows "Claude: <state-or-tool-name>" — ToolUse's
    // detail can be an arbitrary tool name from the transcript, so cap it
    // the same way the status-bar message slot is capped.
    ElidedLabel *m_claudeStatusLabel = nullptr;
    QProgressBar *m_claudeContextBar = nullptr;
    QPushButton *m_claudeReviewBtn = nullptr;
    QLabel *m_claudeErrorLabel = nullptr;
    // 0.6.27 — cached last state/detail so the Claude status label can be
    // re-rendered when the permission-prompt overlay toggles without having
    // to wait for the next ClaudeIntegration::stateChanged tick. When
    // m_claudePromptActive is true, the label shows "Claude: prompting"
    // regardless of the underlying state (user's scrolled-up case from
    // 2026-04-15 — they can't see the prompt directly so the status bar
    // is the only at-a-glance indicator).
    ClaudeState m_claudeLastState = ClaudeState::NotRunning;
    QString m_claudeLastDetail;
    bool m_claudePromptActive = false;
    void applyClaudeStatusLabel();
    void openClaudeAllowlistDialog(const QString &prefillRule = QString());
    void openClaudeProjectsDialog();
    void setupClaudeIntegration();
    void updateClaudeThemeColors();
    void showDiffViewer();
    // Re-check git diff state and enable/disable the Review Changes
    // button accordingly. Async (QProcess) so it never blocks the UI
    // thread. Called on fileChanged events, tab switches, and after
    // the diff viewer finds no changes (to avoid clicking a button
    // that then shows "no changes" again).
    void refreshReviewButton();

    // Plugin manager
#ifdef ANTS_LUA_PLUGINS
    PluginManager *m_pluginManager = nullptr;
    QList<QShortcut *> m_pluginShortcuts; // shortcuts registered via manifest "keybindings"
#endif

    // Tab UUIDs for session persistence
    QHash<QWidget *, QString> m_tabSessionIds;

    // First-show flag (per-instance, not static)
    bool m_firstShow = true;

    // XCB position tracker (Qt's pos()/moveEvent broken for frameless windows on KWin)
    XcbPositionTracker *m_posTracker = nullptr;

    // Hot-reload: watch config.json for external changes
    QFileSystemWatcher *m_configWatcher = nullptr;
    void onConfigFileChanged(const QString &path);

    // Dark/light mode auto-switching
    void onSystemColorSchemeChanged();

    // Auto-profile switching
    void checkAutoProfileRules(TerminalWidget *terminal);
    QString m_lastAutoProfile;

    // Command snippets
    void showSnippetsDialog();

    // Uptime — skip session save if app ran < 5s (test/crash scenario)
    QElapsedTimer m_uptimeTimer;
};
