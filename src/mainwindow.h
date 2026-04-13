#pragma once

#include "config.h"
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
class QSplitter;
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
    QTabWidget *m_tabWidget = nullptr;
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

    // Status bar widgets
    QLabel *m_statusGitBranch = nullptr;
    QLabel *m_statusMessage = nullptr;
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

    // Tab color groups
    QHash<int, QColor> m_tabColors; // tab index -> color
    void showTabColorMenu(int tabIndex);

    // Handle trigger signals from terminals
    void onTriggerFired(const QString &pattern, const QString &actionType, const QString &actionValue);

    // Claude Code integration
    ClaudeIntegration *m_claudeIntegration = nullptr;
    ClaudeAllowlistDialog *m_claudeDialog = nullptr;
    ClaudeProjectsDialog *m_claudeProjects = nullptr;
    ClaudeTranscriptDialog *m_claudeTranscript = nullptr;
    QLabel *m_claudeStatusLabel = nullptr;
    QProgressBar *m_claudeContextBar = nullptr;
    QPushButton *m_claudeReviewBtn = nullptr;
    QLabel *m_claudeErrorLabel = nullptr;
    void openClaudeAllowlistDialog(const QString &prefillRule = QString());
    void openClaudeProjectsDialog();
    void setupClaudeIntegration();
    void updateClaudeThemeColors();
    void showDiffViewer();

    // Plugin manager
#ifdef ANTS_LUA_PLUGINS
    PluginManager *m_pluginManager = nullptr;
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
