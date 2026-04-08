#pragma once

#include "config.h"
#include "themes.h"

#include <QMainWindow>
#include <QTabWidget>
#include <QActionGroup>

class TitleBar;
class TerminalWidget;
class CommandPalette;
class AiDialog;
class SshDialog;
class ClaudeAllowlistDialog;
class QSplitter;
class XcbPositionTracker;
#ifdef ANTS_LUA_PLUGINS
class PluginManager;
#endif

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    bool event(QEvent *event) override;

private slots:
    void onTitleChanged(const QString &title);
    void onShellExited(int code);
    void onImagePasted(const QImage &image);
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

    // Claude Code allowlist
    ClaudeAllowlistDialog *m_claudeDialog = nullptr;
    void openClaudeAllowlistDialog(const QString &prefillRule = QString());

    // Plugin manager
#ifdef ANTS_LUA_PLUGINS
    PluginManager *m_pluginManager = nullptr;
#endif

    // Tab UUIDs for session persistence
    QHash<QWidget *, QString> m_tabSessionIds;

    // XCB position tracker (Qt's pos()/moveEvent broken for frameless windows on KWin)
    XcbPositionTracker *m_posTracker = nullptr;
};
