#include "mainwindow.h"
#include "terminalwidget.h"
#include "titlebar.h"
#include "commandpalette.h"
#include "aidialog.h"
#include "sshdialog.h"
#include "settingsdialog.h"
#include "sessionmanager.h"
#include "xcbpositiontracker.h"
#include "claudeallowlist.h"
#include "claudeintegration.h"
#include "claudeprojects.h"
#include "claudetranscript.h"
#include "auditdialog.h"

#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#endif

#include <algorithm>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolButton>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QProcess>
#include <QFile>
#include <QVBoxLayout>
#include <QTabBar>
#include <QSplitter>
#include <QUuid>
#include <QTimer>
#include <QJsonArray>
#include <QJsonValue>
#include <QFileDialog>
#include <QTextStream>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QTextEdit>
#include <QClipboard>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QCursor>
#include <memory>
#include <QGuiApplication>
#include <QStyleHints>
#include <QInputDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QLineEdit>
#include <QSystemTrayIcon>

MainWindow::MainWindow(bool quakeMode, QWidget *parent) : QMainWindow(parent) {
    m_uptimeTimer.start();
    setWindowTitle("Ants Terminal");
    setWindowFlag(Qt::FramelessWindowHint);

    // Restore window size
    resize(m_config.windowWidth(), m_config.windowHeight());

    // Position tracker — bypasses Qt's broken pos()/moveEvent for frameless windows
    m_posTracker = new XcbPositionTracker(this);

    // Always enable translucent background — on X11, the window visual (RGB vs
    // ARGB) is determined at creation time and cannot be changed after show().
    // Without this, per-pixel alpha (background transparency, window opacity)
    // has no effect when toggled at runtime.
    setAttribute(Qt::WA_TranslucentBackground, true);
    // WA_TranslucentBackground disables auto-fill for the entire widget tree.
    // WA_StyledBackground ensures the QMainWindow's stylesheet background-color
    // still paints, keeping the UI chrome opaque.
    setAttribute(Qt::WA_StyledBackground, true);

    // Custom title bar
    m_titleBar = new TitleBar(this);
    m_titleBar->setTitle("Ants Terminal");
    connect(m_titleBar, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_titleBar, &TitleBar::maximizeRequested, this, &MainWindow::toggleMaximize);
    connect(m_titleBar, &TitleBar::closeRequested, this, &QWidget::close);
    connect(m_titleBar->centerButton(), &QToolButton::clicked, this, &MainWindow::centerWindow);
    connect(m_titleBar, &TitleBar::windowMoved, this, [this](const QPoint &pos) {
        m_posTracker->updatePos(pos);
        m_titleBar->setKnownWindowPos(pos);
        // Save immediately — don't wait for closeEvent
        m_config.setWindowGeometry(pos.x(), pos.y(), width(), height());
    });

    // Standalone menu bar
    m_menuBar = new QMenuBar(this);

    // Tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Tab bar context menu for tab groups (color labels)
    m_tabWidget->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabWidget->tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        int tabIdx = m_tabWidget->tabBar()->tabAt(pos);
        if (tabIdx >= 0) showTabColorMenu(tabIdx);
    });

    // Layout: title bar -> menu bar -> tabs
    QWidget *central = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_titleBar);
    vbox->addWidget(m_menuBar);
    vbox->addWidget(m_tabWidget, 1);
    setCentralWidget(central);

    setupMenus();

    // Command palette (Ctrl+Shift+P)
    m_commandPalette = new CommandPalette(central);
    m_commandPalette->hide();
    connect(m_commandPalette, &CommandPalette::closed, this, [this]() {
        if (auto *t = focusedTerminal()) t->setFocus();
    });
    // Collect all menu actions for the palette
    QList<QAction *> allActions;
    for (QAction *menuAction : m_menuBar->actions()) {
        if (menuAction->menu())
            collectActions(menuAction->menu(), allActions);
    }
    m_commandPalette->setActions(allActions);

#ifdef ANTS_LUA_PLUGINS
    // Initialize plugin system
    QString pluginDir = m_config.pluginDir();
    if (pluginDir.isEmpty()) {
        pluginDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                    + "/ants-terminal/plugins";
    }
    m_pluginManager = new PluginManager(this);
    m_pluginManager->setPluginDir(pluginDir);
    m_pluginManager->scanAndLoad(m_config.enabledPlugins());
    connect(m_pluginManager, &PluginManager::sendToTerminal, this, [this](const QString &text) {
        if (auto *t = focusedTerminal()) t->writeCommand(text);
    });
    connect(m_pluginManager, &PluginManager::statusMessage, this, [this](const QString &msg) {
        showStatusMessage(msg, 5000);
    });
    connect(m_pluginManager, &PluginManager::logMessage, this, [this](const QString &msg) {
        showStatusMessage("Plugin: " + msg, 3000);
    });
#endif

    // Apply saved theme
    applyTheme(m_config.theme());

    // Claude Code integration — must be set up BEFORE newTab(), otherwise
    // the null-guarded m_claudeIntegration->setShellPid() call in newTab()
    // is a no-op for the first tab and polling never starts (so the
    // Claude status widget never appears in the status bar).
    setupClaudeIntegration();

    // Create first tab (restoreSessions may replace it if there are saved sessions)
    newTab();

    // Restore saved sessions from previous run
    restoreSessions();

    // Status bar info widgets (git branch, status message, process).
    // Transient status messages go into m_statusMessage (not statusBar()->showMessage),
    // so the git branch label stays visible to the left of them.
    m_statusGitBranch = new QLabel(this);
    statusBar()->addWidget(m_statusGitBranch);

    m_statusMessage = new QLabel(this);
    statusBar()->addWidget(m_statusMessage, 1);

    m_statusProcess = new QLabel(this);
    statusBar()->addWidget(m_statusProcess);

    // Status update timer (every 2 seconds)
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateTabTitles);
    m_statusTimer->start();

    // Broadcast mode from config
    m_broadcastMode = m_config.broadcastMode();

    // Quake mode (from config or constructor flag)
    if (quakeMode || m_config.quakeMode())
        setupQuakeMode();

    // Hot-reload: watch config.json for external changes
    m_configWatcher = new QFileSystemWatcher(this);
    m_configWatcher->addPath(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                             + "/ants-terminal/config.json");
    connect(m_configWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::onConfigFileChanged);

    // Dark/light mode auto-switching (Qt 6.5+ signal)
    if (m_config.autoColorScheme()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this]() { onSystemColorSchemeChanged(); });
        // Apply initial scheme
        onSystemColorSchemeChanged();
    }

    // Cleanup old sessions at startup, then once every 24 h for long-running
    // instances (desktop-wide Quake tile, tmux-like usage).
    SessionManager::cleanupOldSessions(30);
    auto *sessionCleanupTimer = new QTimer(this);
    sessionCleanupTimer->setInterval(24 * 60 * 60 * 1000);
    connect(sessionCleanupTimer, &QTimer::timeout, this, []() {
        SessionManager::cleanupOldSessions(30);
    });
    sessionCleanupTimer->start();
}

void MainWindow::setupMenus() {
    // File menu
    QMenu *fileMenu = m_menuBar->addMenu("&File");

    QAction *newTabAction = fileMenu->addAction("New &Tab");
    newTabAction->setShortcut(QKeySequence(m_config.keybinding("new_tab", "Ctrl+Shift+T")));
    connect(newTabAction, &QAction::triggered, this, &MainWindow::newTab);

    QAction *closeTabAction = fileMenu->addAction("&Close Tab");
    closeTabAction->setShortcut(QKeySequence(m_config.keybinding("close_tab", "Ctrl+Shift+W")));
    connect(closeTabAction, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    QAction *undoCloseAction = fileMenu->addAction("&Undo Close Tab");
    undoCloseAction->setShortcut(QKeySequence(m_config.keybinding("undo_close_tab", "Ctrl+Shift+Z")));
    connect(undoCloseAction, &QAction::triggered, this, [this]() {
        if (m_closedTabs.isEmpty()) {
            showStatusMessage("No closed tabs to restore", 3000);
            return;
        }
        ClosedTabInfo info = m_closedTabs.takeFirst();
        newTab();
        // cd to the previous working directory (shell-quote to prevent injection)
        if (!info.cwd.isEmpty()) {
            if (auto *t = focusedTerminal()) {
                QString escaped = info.cwd;
                escaped.replace("'", "'\\''");
                t->writeCommand("cd '" + escaped + "'\n");
            }
        }
        showStatusMessage("Restored tab: " + info.title, 3000);
    });

    QAction *nextTabAction = fileMenu->addAction("Ne&xt Tab");
    nextTabAction->setShortcut(QKeySequence(m_config.keybinding("next_tab", "Ctrl+PgDown")));
    connect(nextTabAction, &QAction::triggered, this, [this]() {
        int next = m_tabWidget->currentIndex() + 1;
        if (next >= m_tabWidget->count()) next = 0;
        m_tabWidget->setCurrentIndex(next);
    });

    QAction *prevTabAction = fileMenu->addAction("Pre&v Tab");
    prevTabAction->setShortcut(QKeySequence(m_config.keybinding("prev_tab", "Ctrl+PgUp")));
    connect(prevTabAction, &QAction::triggered, this, [this]() {
        int prev = m_tabWidget->currentIndex() - 1;
        if (prev < 0) prev = m_tabWidget->count() - 1;
        m_tabWidget->setCurrentIndex(prev);
    });

    // Alt+1..9 to jump to tab by index
    for (int i = 1; i <= 9; ++i) {
        auto *a = new QAction(this);
        a->setShortcut(QKeySequence(QString("Alt+%1").arg(i)));
        connect(a, &QAction::triggered, this, [this, i]() {
            int idx = (i == 9) ? m_tabWidget->count() - 1 : i - 1;
            if (idx >= 0 && idx < m_tabWidget->count())
                m_tabWidget->setCurrentIndex(idx);
        });
        addAction(a);
    }

    fileMenu->addSeparator();

    QAction *newWindowAction = fileMenu->addAction("&New Window");
    newWindowAction->setShortcut(QKeySequence(m_config.keybinding("new_window", "Ctrl+Shift+N")));
    connect(newWindowAction, &QAction::triggered, this, []() {
        auto *win = new MainWindow();
        win->setAttribute(Qt::WA_DeleteOnClose);
        win->show();
    });

    fileMenu->addSeparator();

    // SSH Manager
    QAction *sshAction = fileMenu->addAction("SSH &Manager...");
    sshAction->setShortcut(QKeySequence(m_config.keybinding("ssh_manager", "Ctrl+Shift+S")));
    connect(sshAction, &QAction::triggered, this, [this]() {
        if (!m_sshDialog) {
            m_sshDialog = new SshDialog(this);
            connect(m_sshDialog, &SshDialog::connectRequested,
                    this, &MainWindow::onSshConnect);
            connect(m_sshDialog, &SshDialog::bookmarksChanged,
                    this, [this](const QList<SshBookmark> &bookmarks) {
                QJsonArray arr;
                for (const auto &bm : bookmarks)
                    arr.append(bm.toJson());
                m_config.setSshBookmarksJson(arr);
            });
        }
        // Load saved bookmarks
        QList<SshBookmark> bookmarks;
        QJsonArray arr = m_config.sshBookmarksJson();
        for (const QJsonValue &v : arr)
            bookmarks.append(SshBookmark::fromJson(v.toObject()));
        m_sshDialog->setBookmarks(bookmarks);
        m_sshDialog->show();
        m_sshDialog->raise();
    });

    fileMenu->addSeparator();

    QAction *exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence(m_config.keybinding("exit", "Ctrl+Shift+Q")));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // Edit menu
    QMenu *editMenu = m_menuBar->addMenu("&Edit");

    QAction *richCopyAction = editMenu->addAction("Copy with &Colors");
    richCopyAction->setShortcut(QKeySequence(m_config.keybinding("rich_copy", "Ctrl+Shift+Alt+C")));
    connect(richCopyAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->copySelectionRich();
    });

    editMenu->addSeparator();

    QAction *clearLineAction = editMenu->addAction("Clear &Input Line");
    clearLineAction->setShortcut(QKeySequence(m_config.keybinding("clear_line", "Ctrl+Shift+U")));
    connect(clearLineAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->sendToPty(QByteArray("\x01\x0B", 2)); // Ctrl+A + Ctrl+K
    });

    // View menu
    QMenu *viewMenu = m_menuBar->addMenu("&View");

    // Themes submenu
    QMenu *themesMenu = viewMenu->addMenu("&Themes");
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);
    for (const QString &name : Themes::names()) {
        QAction *a = themesMenu->addAction(name);
        a->setCheckable(true);
        a->setChecked(name == m_config.theme());
        m_themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, name]() {
            applyTheme(name);
        });
    }

    viewMenu->addSeparator();

    QAction *zoomIn = viewMenu->addAction("Zoom &In");
    zoomIn->setShortcut(QKeySequence("Ctrl+="));
    connect(zoomIn, &QAction::triggered, this, [this]() { changeFontSize(1); });

    QAction *zoomOut = viewMenu->addAction("Zoom &Out");
    zoomOut->setShortcut(QKeySequence("Ctrl+-"));
    connect(zoomOut, &QAction::triggered, this, [this]() { changeFontSize(-1); });

    QAction *zoomReset = viewMenu->addAction("&Reset Zoom");
    zoomReset->setShortcut(QKeySequence("Ctrl+0"));
    connect(zoomReset, &QAction::triggered, this, [this]() {
        m_config.setFontSize(11);
        applyFontSizeToAll(11);
    });

    viewMenu->addSeparator();

    // Opacity submenu
    QMenu *opacityMenu = viewMenu->addMenu("&Opacity");
    m_opacityGroup = new QActionGroup(this);
    m_opacityGroup->setExclusive(true);
    int currentOpacityPct = static_cast<int>(m_config.opacity() * 100 + 0.5);
    for (int pct : {100, 95, 90, 85, 80, 70}) {
        QAction *a = opacityMenu->addAction(QString("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(pct == currentOpacityPct);
        m_opacityGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, pct]() {
            double val = pct / 100.0;
            m_config.setOpacity(val);
            // Re-apply theme to update all background colors with new opacity
            applyTheme(m_currentTheme);
        });
    }

    viewMenu->addSeparator();

    QAction *centerAction = viewMenu->addAction("&Center Window");
    centerAction->setShortcut(QKeySequence("Ctrl+Shift+M"));
    connect(centerAction, &QAction::triggered, this, &MainWindow::centerWindow);

    viewMenu->addSeparator();

    QAction *paletteAction = viewMenu->addAction("Command &Palette");
    paletteAction->setShortcut(QKeySequence(m_config.keybinding("command_palette", "Ctrl+Shift+P")));
    connect(paletteAction, &QAction::triggered, this, [this]() {
        if (m_commandPalette) m_commandPalette->show();
    });

    // OSC 133 prompt navigation — discoverable in menu + command palette.
    // Works when the shell emits OSC 133 A/B/C markers (bash/zsh/fish integration).
    // No shortcut here: TerminalWidget::keyPressEvent intercepts Ctrl+Shift+Up/Down
    // directly (before Qt dispatches to menu shortcuts), which is why the label
    // shows the hint inline rather than relying on Qt's QAction shortcut display.
    QAction *prevPromptAction = viewMenu->addAction("Previous &Prompt\tCtrl+Shift+Up");
    connect(prevPromptAction, &QAction::triggered, this, [this]() {
        if (auto *t = focusedTerminal()) t->navigatePrompt(-1);
    });

    QAction *nextPromptAction = viewMenu->addAction("Next P&rompt\tCtrl+Shift+Down");
    connect(nextPromptAction, &QAction::triggered, this, [this]() {
        if (auto *t = focusedTerminal()) t->navigatePrompt(1);
    });

    viewMenu->addSeparator();

    // Reload user themes
    QAction *reloadThemes = viewMenu->addAction("Reload &User Themes");
    connect(reloadThemes, &QAction::triggered, this, [this, themesMenu]() {
        Themes::reload();
        // Clear old actions from group before rebuilding
        for (auto *a : m_themeGroup->actions())
            m_themeGroup->removeAction(a);
        themesMenu->clear();
        for (const QString &name : Themes::names()) {
            QAction *a = themesMenu->addAction(name);
            a->setCheckable(true);
            a->setChecked(name == m_currentTheme);
            m_themeGroup->addAction(a);
            connect(a, &QAction::triggered, this, [this, name]() {
                applyTheme(name);
            });
        }
        showStatusMessage("Themes reloaded", 3000);
    });

    // Performance overlay
    QAction *perfAction = viewMenu->addAction("Performance &Overlay");
    perfAction->setShortcut(QKeySequence("Ctrl+Shift+F12"));
    perfAction->setCheckable(true);
    connect(perfAction, &QAction::toggled, this, [this](bool checked) {
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setShowPerformanceOverlay(checked);
    });

    // Background image
    QAction *bgImageAction = viewMenu->addAction("Set &Background Image...");
    connect(bgImageAction, &QAction::triggered, this, [this]() {
        QString path = QFileDialog::getOpenFileName(this, "Select Background Image", QString(),
                                                     "Images (*.png *.jpg *.jpeg *.bmp *.webp)");
        if (path.isEmpty()) {
            // Clear background image
            m_config.setBackgroundImage("");
            QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
            for (auto *t : terminals) t->setBackgroundImage("");
            showStatusMessage("Background image cleared", 3000);
        } else {
            m_config.setBackgroundImage(path);
            QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
            for (auto *t : terminals) t->setBackgroundImage(path);
            showStatusMessage("Background image set", 3000);
        }
    });

    // Split menu
    QMenu *splitMenu = m_menuBar->addMenu("&Split");

    QAction *splitH = splitMenu->addAction("Split &Horizontal");
    splitH->setShortcut(QKeySequence(m_config.keybinding("split_horizontal", "Ctrl+Shift+E")));
    connect(splitH, &QAction::triggered, this, &MainWindow::splitHorizontal);

    QAction *splitV = splitMenu->addAction("Split &Vertical");
    splitV->setShortcut(QKeySequence(m_config.keybinding("split_vertical", "Ctrl+Shift+O")));
    connect(splitV, &QAction::triggered, this, &MainWindow::splitVertical);

    QAction *closePane = splitMenu->addAction("&Close Pane");
    closePane->setShortcut(QKeySequence(m_config.keybinding("close_pane", "Ctrl+Shift+X")));
    connect(closePane, &QAction::triggered, this, &MainWindow::closeFocusedPane);

    // Tools menu
    QMenu *toolsMenu = m_menuBar->addMenu("&Tools");

    // AI Assistant
    QAction *aiAction = toolsMenu->addAction("&AI Assistant...");
    aiAction->setShortcut(QKeySequence(m_config.keybinding("ai_assistant", "Ctrl+Shift+A")));
    connect(aiAction, &QAction::triggered, this, [this]() {
        if (!m_aiDialog) {
            m_aiDialog = new AiDialog(this);
            connect(m_aiDialog, &AiDialog::insertCommand, this, [this](const QString &cmd) {
                if (auto *t = focusedTerminal()) t->writeCommand(cmd);
            });
            connect(m_aiDialog, &QDialog::finished, this, [this]() {
                if (auto *t = focusedTerminal()) t->setFocus();
            });
        }
        // Update context and config
        if (auto *t = focusedTerminal()) {
            m_aiDialog->setTerminalContext(t->recentOutput(m_config.aiContextLines()));
        }
        m_aiDialog->setConfig(m_config.aiEndpoint(), m_config.aiApiKey(),
                              m_config.aiModel(), m_config.aiContextLines());
        m_aiDialog->show();
        m_aiDialog->raise();
    });

    toolsMenu->addSeparator();

    // Project Audit
    QAction *auditAction = toolsMenu->addAction("Project &Audit...");
    connect(auditAction, &QAction::triggered, this, [this]() {
        QString cwd;
        if (auto *t = focusedTerminal()) cwd = t->shellCwd();
        if (cwd.isEmpty()) cwd = QDir::currentPath();
        auto *dlg = new AuditDialog(cwd, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &AuditDialog::reviewRequested, this, [this](const QString &resultsFile) {
            auto *t = focusedTerminal();
            if (!t) t = currentTerminal();
            if (!t) return;
            // Send a Claude Code command that reads the audit file and fixes issues
            QString cmd = QString("claude \"Read %1 and fix any real issues found in the project audit."
                                  " Focus on bugs, security vulnerabilities, and code quality problems."
                                  " Ignore informational items like line counts and file sizes."
                                  " For each fix, explain what you changed and why.\"\n").arg(resultsFile);
            t->writeCommand(cmd);
        });
        dlg->show();
    });

    toolsMenu->addSeparator();

    // Claude Code submenu
    QMenu *claudeMenu = toolsMenu->addMenu("&Claude Code");

    QAction *editAllowlist = claudeMenu->addAction("Edit &Allowlist...");
    editAllowlist->setShortcut(QKeySequence(m_config.keybinding("claude_allowlist", "Ctrl+Shift+L")));
    connect(editAllowlist, &QAction::triggered, this, [this]() {
        openClaudeAllowlistDialog();
    });

    QAction *viewProjects = claudeMenu->addAction("&Projects && Sessions...");
    viewProjects->setShortcut(QKeySequence(m_config.keybinding("claude_projects", "Ctrl+Shift+J")));
    connect(viewProjects, &QAction::triggered, this, [this]() {
        openClaudeProjectsDialog();
    });

    QAction *viewTranscript = claudeMenu->addAction("View &Transcript...");
    connect(viewTranscript, &QAction::triggered, this, [this]() {
        if (!m_claudeTranscript) {
            m_claudeTranscript = new ClaudeTranscriptDialog(m_claudeIntegration, this);
            connect(m_claudeTranscript, &QDialog::finished, this, [this]() {
                if (auto *t = focusedTerminal()) t->setFocus();
            });
        }
        m_claudeTranscript->show();
        m_claudeTranscript->raise();
    });

    claudeMenu->addSeparator();

    // Slash command shortcuts
    for (auto &[label, cmd] : std::initializer_list<std::pair<const char*, const char*>>{
        {"Send /compact", "/compact"},
        {"Send /clear", "/clear"},
        {"Send /cost", "/cost"},
        {"Send /help", "/help"},
        {"Send /status", "/status"},
    }) {
        QAction *a = claudeMenu->addAction(label);
        connect(a, &QAction::triggered, this, [this, cmd]() {
            if (auto *t = focusedTerminal()) t->writeCommand(QString(cmd));
        });
    }

    claudeMenu->addSeparator();

    // Model switching submenu
    QMenu *modelMenu = claudeMenu->addMenu("Switch &Model");
    for (auto &[label, cmd] : std::initializer_list<std::pair<const char*, const char*>>{
        {"Opus (most capable)", "/model opus"},
        {"Sonnet (fast + capable)", "/model sonnet"},
        {"Haiku (fastest)", "/model haiku"},
    }) {
        QAction *a = modelMenu->addAction(label);
        connect(a, &QAction::triggered, this, [this, cmd]() {
            if (auto *t = focusedTerminal()) t->writeCommand(QString(cmd));
        });
    }

    // Thinking level submenu
    QMenu *thinkMenu = claudeMenu->addMenu("Thinking &Level");
    for (auto &[label, cmd] : std::initializer_list<std::pair<const char*, const char*>>{
        {"Ultra Think", "/ultrathink"},
        {"Think", "/think"},
        {"No Think", "/nothink"},
    }) {
        QAction *a = thinkMenu->addAction(label);
        connect(a, &QAction::triggered, this, [this, cmd]() {
            if (auto *t = focusedTerminal()) t->writeCommand(QString(cmd));
        });
    }

    // Review Changes action
    QAction *reviewChanges = claudeMenu->addAction("&Review Changes...");
    connect(reviewChanges, &QAction::triggered, this, &MainWindow::showDiffViewer);

    // Tools: Scratchpad
    toolsMenu->addSeparator();
    QAction *scratchpadAction = toolsMenu->addAction("&Scratchpad Editor...");
    scratchpadAction->setShortcut(QKeySequence(m_config.keybinding("scratchpad", "Ctrl+Shift+Return")));
    connect(scratchpadAction, &QAction::triggered, this, [this]() {
        if (auto *t = focusedTerminal()) t->showScratchpad();
    });

    // Tools: Command Snippets
    QAction *snippetsAction = toolsMenu->addAction("Command Sni&ppets...");
    snippetsAction->setShortcut(QKeySequence(m_config.keybinding("snippets", "Ctrl+Shift+;")));
    connect(snippetsAction, &QAction::triggered, this, &MainWindow::showSnippetsDialog);

    // Tools: Fold/Unfold command output
    QAction *foldAction = toolsMenu->addAction("Toggle &Fold Output");
    foldAction->setShortcut(QKeySequence(m_config.keybinding("toggle_fold", "Ctrl+Shift+.")));
    connect(foldAction, &QAction::triggered, this, [this]() {
        if (auto *t = focusedTerminal()) t->toggleFoldAtCursor();
    });

    // Settings menu
    QMenu *settingsMenu = m_menuBar->addMenu("S&ettings");

    QAction *loggingAction = settingsMenu->addAction("Session &Logging");
    loggingAction->setCheckable(true);
    loggingAction->setChecked(m_config.sessionLogging());
    connect(loggingAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setSessionLogging(checked);
        // Apply to all terminals
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setSessionLogging(checked);
        showStatusMessage(checked ? "Session logging enabled" : "Session logging disabled", 3000);
    });

    QAction *autoCopyAction = settingsMenu->addAction("&Auto-copy on Select");
    autoCopyAction->setCheckable(true);
    autoCopyAction->setChecked(m_config.autoCopyOnSelect());
    connect(autoCopyAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setAutoCopyOnSelect(checked);
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setAutoCopyOnSelect(checked);
    });

    QAction *bellAction = settingsMenu->addAction("&Visual Bell");
    bellAction->setCheckable(true);
    bellAction->setChecked(m_config.visualBell());
    connect(bellAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setVisualBell(checked);
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setVisualBell(checked);
        showStatusMessage(checked ? "Visual bell enabled" : "Visual bell disabled", 3000);
    });

    QAction *blurAction = settingsMenu->addAction("Background &Blur");
    blurAction->setCheckable(true);
    blurAction->setChecked(m_config.backgroundBlur());
    connect(blurAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setBackgroundBlur(checked);
        // WA_TranslucentBackground is always set at construction time
        showStatusMessage(checked ? "Blur enabled (restart for full effect)" : "Blur disabled", 3000);
    });

    // GPU rendering toggle
    QAction *gpuAction = settingsMenu->addAction("&GPU Rendering");
    gpuAction->setCheckable(true);
    gpuAction->setChecked(m_config.gpuRendering());
    connect(gpuAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setGpuRendering(checked);
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setGpuRendering(checked);
        showStatusMessage(checked ? "GPU rendering enabled" : "GPU rendering disabled", 3000);
    });

    // Session persistence toggle
    QAction *persistAction = settingsMenu->addAction("Session &Persistence");
    persistAction->setCheckable(true);
    persistAction->setChecked(m_config.sessionPersistence());
    connect(persistAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setSessionPersistence(checked);
        showStatusMessage(checked ? "Session persistence enabled" : "Session persistence disabled", 3000);
    });

    settingsMenu->addSeparator();

    QAction *recordAction = settingsMenu->addAction("&Record Session");
    recordAction->setCheckable(true);
    recordAction->setShortcut(QKeySequence(m_config.keybinding("record_session", "Ctrl+Shift+R")));
    connect(recordAction, &QAction::toggled, this, [this](bool checked) {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (!t) return;
        if (checked) {
            QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                          + "/ants-terminal/recordings";
            QDir().mkpath(dir);
            QString path = dir + "/recording_"
                + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".cast";
            t->startRecording(path);
            showStatusMessage("Recording: " + path, 5000);
        } else {
            t->stopRecording();
            showStatusMessage("Recording stopped", 3000);
        }
    });

    settingsMenu->addSeparator();

    // Bookmarks
    QAction *bookmarkAction = settingsMenu->addAction("Toggle &Bookmark");
    bookmarkAction->setShortcut(QKeySequence(m_config.keybinding("toggle_bookmark", "Ctrl+Shift+B")));
    connect(bookmarkAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->toggleBookmark();
    });

    QAction *nextBmAction = settingsMenu->addAction("Next Bookmark");
    nextBmAction->setShortcut(QKeySequence(m_config.keybinding("next_bookmark", "Ctrl+Shift+Down")));
    connect(nextBmAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->nextBookmark();
    });

    QAction *prevBmAction = settingsMenu->addAction("Previous Bookmark");
    prevBmAction->setShortcut(QKeySequence(m_config.keybinding("prev_bookmark", "Ctrl+Shift+Up")));
    connect(prevBmAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->prevBookmark();
    });

    QAction *urlSelectAction = settingsMenu->addAction("Quick Select &URL");
    urlSelectAction->setShortcut(QKeySequence(m_config.keybinding("url_quick_select", "Ctrl+Shift+G")));
    connect(urlSelectAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->enterUrlQuickSelect();
    });

    settingsMenu->addSeparator();

    // Scrollback submenu
    QMenu *scrollbackMenu = settingsMenu->addMenu("Scrollback &Lines");
    m_scrollbackGroup = new QActionGroup(this);
    m_scrollbackGroup->setExclusive(true);
    int currentScrollback = m_config.scrollbackLines();
    for (int lines : {10000, 50000, 100000, 500000}) {
        QAction *a = scrollbackMenu->addAction(QString::number(lines));
        a->setCheckable(true);
        a->setChecked(lines == currentScrollback);
        m_scrollbackGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, lines]() {
            m_config.setScrollbackLines(lines);
            QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
            for (auto *t : terminals) t->setMaxScrollback(lines);
            showStatusMessage(QString("Scrollback: %1 lines").arg(lines), 3000);
        });
    }

    settingsMenu->addSeparator();

    // Broadcast input toggle
    m_broadcastAction = settingsMenu->addAction("&Broadcast Input to All Panes");
    m_broadcastAction->setCheckable(true);
    m_broadcastAction->setChecked(m_config.broadcastMode());
    m_broadcastAction->setShortcut(QKeySequence(m_config.keybinding("broadcast_input", "Ctrl+Shift+I")));
    connect(m_broadcastAction, &QAction::toggled, this, [this](bool checked) {
        m_broadcastMode = checked;
        m_config.setBroadcastMode(checked);
        showStatusMessage(checked ? "Broadcast mode ON — input sent to all panes"
                                         : "Broadcast mode OFF", 3000);
    });

    settingsMenu->addSeparator();

    // Export scrollback
    QAction *exportAction = settingsMenu->addAction("Export Scro&llback...");
    connect(exportAction, &QAction::triggered, this, [this]() {
        auto *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (!t) return;
        // Trigger the context menu export (reuses same code)
        QString path = QFileDialog::getSaveFileName(this, "Export Scrollback", QString(),
                                                     "Text Files (*.txt);;HTML Files (*.html)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream stream(&file);
        if (path.endsWith(".html", Qt::CaseInsensitive))
            stream << t->exportAsHtml();
        else
            stream << t->exportAsText();
        showStatusMessage("Scrollback exported to " + path, 5000);
    });

    settingsMenu->addSeparator();

    // Settings dialog
    QAction *settingsAction = settingsMenu->addAction("&Preferences...");
    settingsAction->setShortcut(QKeySequence(m_config.keybinding("preferences", "Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, [this]() {
        if (!m_settingsDialog) {
            m_settingsDialog = new SettingsDialog(&m_config, this);
            connect(m_settingsDialog, &SettingsDialog::settingsChanged, this, [this]() {
                // Apply all changed settings
                applyTheme(m_config.theme());
                applyFontSizeToAll(m_config.fontSize());

                QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
                for (auto *t : terminals) {
                    applyConfigToTerminal(t);
                    t->setHighlightRules(m_config.highlightRules());
                    t->setTriggerRules(m_config.triggerRules());
                    QString family = m_config.fontFamily();
                    if (!family.isEmpty()) t->setFontFamily(family);
                }

                // Opacity is now applied via per-pixel alpha in applyTheme() above

                // Update broadcast
                m_broadcastMode = m_config.broadcastMode();
                if (m_broadcastAction)
                    m_broadcastAction->setChecked(m_broadcastMode);

                // Update quake mode
                if (m_config.quakeMode() && !m_quakeMode)
                    setupQuakeMode();

                showStatusMessage("Settings applied", 3000);
            });
            connect(m_settingsDialog, &QDialog::finished, this, [this]() {
                if (auto *t = focusedTerminal()) t->setFocus();
            });
        }
        m_settingsDialog->show();
        m_settingsDialog->raise();
    });

#ifdef ANTS_LUA_PLUGINS
    // Plugins menu
    settingsMenu->addSeparator();
    QAction *reloadPluginsAction = settingsMenu->addAction("Reload &Plugins");
    connect(reloadPluginsAction, &QAction::triggered, this, [this]() {
        m_pluginManager->reloadAll(m_config.enabledPlugins());
        showStatusMessage(
            QString("Loaded %1 plugins").arg(m_pluginManager->pluginCount()), 3000);
    });
#endif
}

TerminalWidget *MainWindow::createTerminal() {
    auto *terminal = new TerminalWidget();
    applyConfigToTerminal(terminal);

    if (!m_currentTheme.isEmpty()) {
        const Theme &theme = Themes::byName(m_currentTheme);
        terminal->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor,
                                    theme.accent, theme.border);
    }

    return terminal;
}

void MainWindow::applyConfigToTerminal(TerminalWidget *terminal) {
    terminal->setFontSize(m_config.fontSize());
    terminal->setMaxScrollback(m_config.scrollbackLines());
    terminal->setSessionLogging(m_config.sessionLogging());
    terminal->setAutoCopyOnSelect(m_config.autoCopyOnSelect());
    terminal->setEditorCommand(m_config.editorCommand());
    terminal->setImagePasteDir(m_config.imagePasteDir());
    terminal->setWindowOpacityLevel(m_config.opacity());
    terminal->setGpuRendering(m_config.gpuRendering());
    terminal->setVisualBell(m_config.visualBell());
    terminal->setPadding(m_config.terminalPadding());
    QString family = m_config.fontFamily();
    if (!family.isEmpty()) terminal->setFontFamily(family);
    // Per-style fonts
    QString boldFamily = m_config.boldFontFamily();
    if (!boldFamily.isEmpty()) terminal->setBoldFontFamily(boldFamily);
    QString italicFamily = m_config.italicFontFamily();
    if (!italicFamily.isEmpty()) terminal->setItalicFontFamily(italicFamily);
    QString biFamily = m_config.boldItalicFontFamily();
    if (!biFamily.isEmpty()) terminal->setBoldItalicFontFamily(biFamily);
    // Background image
    QString bgImg = m_config.backgroundImage();
    if (!bgImg.isEmpty()) terminal->setBackgroundImage(bgImg);
    // Badge text
    QString badge = m_config.badgeText();
    if (!badge.isEmpty()) terminal->setBadgeText(badge);
}

void MainWindow::connectTerminal(TerminalWidget *terminal) {
    connect(terminal, &TerminalWidget::titleChanged, this, [this, terminal](const QString &title) {
        // Find which tab this terminal is in
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (m_tabWidget->widget(i)->isAncestorOf(terminal) || m_tabWidget->widget(i) == terminal) {
                QString tabTitle = title.isEmpty() ? "Shell" : title;
                if (tabTitle.length() > 30)
                    tabTitle = tabTitle.left(27) + "...";
                m_tabWidget->setTabText(i, tabTitle);
                break;
            }
        }
        if (terminal == focusedTerminal()) {
            onTitleChanged(title);
        }
    });

    connect(terminal, &TerminalWidget::shellExited, this, [this, terminal](int /*code*/) {
        // Find parent splitter
        QSplitter *splitter = findParentSplitter(terminal);
        if (splitter) {
            terminal->setParent(nullptr);
            terminal->deleteLater();
            cleanupEmptySplitters(m_tabWidget->currentWidget());
        } else {
            // It's the only terminal in the tab
            int idx = m_tabWidget->indexOf(terminal);
            if (idx >= 0) closeTab(idx);
        }
    });

    // Broadcast callback
    terminal->setBroadcastCallback([this](TerminalWidget *source, const QByteArray &data) {
        if (!m_broadcastMode) return;
        QList<TerminalWidget *> all = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : all) {
            if (t != source) t->sendToPty(data);
        }
    });

    // Trigger signals
    connect(terminal, &TerminalWidget::triggerFired, this, &MainWindow::onTriggerFired);

    // Desktop notifications (OSC 9/777)
    connect(terminal, &TerminalWidget::desktopNotification, this,
            [this](const QString &title, const QString &body) {
        // Only show notification if window is not focused (avoid distracting the user)
        if (!isActiveWindow()) {
            auto *tray = QSystemTrayIcon::isSystemTrayAvailable()
                ? findChild<QSystemTrayIcon *>() : nullptr;
            if (tray) {
                tray->showMessage(title.isEmpty() ? "Ants Terminal" : title, body);
            } else {
                // Fallback: use notify-send
                QProcess::startDetached("notify-send", {
                    title.isEmpty() ? "Ants Terminal" : title, body
                });
            }
        }
    });

    // Apply highlight and trigger rules from config
    terminal->setHighlightRules(m_config.highlightRules());
    terminal->setTriggerRules(m_config.triggerRules());

    // Error detection — show failed command in status bar
    connect(terminal, &TerminalWidget::commandFailed, this, [this](int exitCode, const QString &output) {
        if (m_claudeErrorLabel) {
            m_claudeErrorLabel->setText(QString("Exit %1").arg(exitCode));
            m_claudeErrorLabel->setToolTip(output.left(500));
            m_claudeErrorLabel->show();
            QTimer::singleShot(10000, m_claudeErrorLabel, &QWidget::hide);
        }
    });

    // Notify Claude integration of terminal output for activity detection
    connect(terminal, &TerminalWidget::outputReceived, this, [this]() {
        if (m_claudeIntegration)
            m_claudeIntegration->notifyTerminalOutput();
    });

    // Claude Code permission detection → status bar notification
    connect(terminal, &TerminalWidget::claudePermissionDetected, this, [this, terminal](const QString &rawRule) {
        // Only show for the currently active tab
        if (terminal != focusedTerminal() && terminal != currentTerminal()) return;

        // Normalize and generalize the detected rule
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;

        // Remove any existing allowlist button to prevent accumulation
        auto existing = statusBar()->findChildren<QPushButton *>("claudeAllowBtn");
        for (auto *btn : existing) btn->deleteLater();

        showStatusMessage(
            QString("Claude Code permission: %1 — ").arg(rule), 0);
        auto *addBtn = new QPushButton("Add to allowlist", statusBar());
        addBtn->setObjectName("claudeAllowBtn");
        statusBar()->addPermanentWidget(addBtn);
        connect(addBtn, &QPushButton::clicked, this, [this, rule, addBtn]() {
            openClaudeAllowlistDialog(rule);
            addBtn->deleteLater();
            clearStatusMessage();
        });

        // Remove button when prompt goes away (new terminal output after grace period)
        QTimer::singleShot(1000, addBtn, [terminal, addBtn]() {
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(terminal, &TerminalWidget::outputReceived, addBtn, [addBtn, conn]() {
                QObject::disconnect(*conn);
                addBtn->deleteLater();
            });
        });
    });
}

void MainWindow::newTab() {
    // Inherit CWD from the currently focused terminal
    QString inheritCwd;
    if (auto *prev = focusedTerminal())
        inheritCwd = prev->shellCwd();
    else if (auto *prev = currentTerminal())
        inheritCwd = prev->shellCwd();

    auto *terminal = createTerminal();
    connectTerminal(terminal);

    QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);
    m_tabSessionIds[terminal] = tabId;

    if (!terminal->startShell(inheritCwd)) {
        showStatusMessage("Failed to start shell!");
    }

    terminal->setFocus();

    // Track shell process for Claude Code integration
    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(terminal->shellPid());

    // Hide tab bar when only one tab
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);
}

void MainWindow::onSshConnect(const QString &sshCommand, bool inNewTab) {
    if (inNewTab) {
        newTab();
    }
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (t) {
        // Small delay to let shell start
        QTimer::singleShot(200, this, [t, sshCommand]() {
            t->writeCommand(sshCommand);
        });
    }
}

void MainWindow::splitCurrentPane(Qt::Orientation orientation) {
    TerminalWidget *current = focusedTerminal();
    if (!current) current = currentTerminal();
    if (!current) return;

    // Create new terminal
    auto *newTerm = createTerminal();
    connectTerminal(newTerm);

    QWidget *parent = current->parentWidget();
    QSplitter *parentSplitter = qobject_cast<QSplitter *>(parent);

    if (parentSplitter) {
        // Already in a splitter — add new pane alongside current
        int idx = parentSplitter->indexOf(current);
        if (parentSplitter->orientation() == orientation) {
            // Same orientation — just insert next to it
            parentSplitter->insertWidget(idx + 1, newTerm);
        } else {
            // Different orientation — need to nest a new splitter
            auto *newSplitter = new QSplitter(orientation);
            parentSplitter->insertWidget(idx, newSplitter);
            current->setParent(nullptr);
            newSplitter->addWidget(current);
            newSplitter->addWidget(newTerm);
        }
    } else {
        // Current terminal is the direct tab widget content
        int tabIdx = m_tabWidget->indexOf(current);
        if (tabIdx < 0) return;

        auto *splitter = new QSplitter(orientation);

        // Transfer session ID from the terminal to the splitter
        QString sessionId = m_tabSessionIds.value(current);
        if (!sessionId.isEmpty()) {
            m_tabSessionIds.remove(current);
            m_tabSessionIds[splitter] = sessionId;
        }

        current->setParent(nullptr);
        splitter->addWidget(current);
        splitter->addWidget(newTerm);

        m_tabWidget->removeTab(tabIdx);
        m_tabWidget->insertTab(tabIdx, splitter, "Shell");
        m_tabWidget->setCurrentIndex(tabIdx);
    }

    if (!newTerm->startShell()) {
        showStatusMessage("Failed to start shell!");
    }
    newTerm->setFocus();
}

void MainWindow::splitHorizontal() {
    splitCurrentPane(Qt::Vertical); // Vertical splitter = horizontal split (panes stacked)
}

void MainWindow::splitVertical() {
    splitCurrentPane(Qt::Horizontal); // Horizontal splitter = vertical split (panes side by side)
}

void MainWindow::closeFocusedPane() {
    TerminalWidget *focused = focusedTerminal();
    if (!focused) return;

    QSplitter *parent = findParentSplitter(focused);
    if (!parent) {
        // Only terminal in the tab -- close tab
        closeCurrentTab();
        return;
    }

    focused->setParent(nullptr);
    focused->deleteLater();
    cleanupEmptySplitters(m_tabWidget->currentWidget());

    // Focus the next available terminal
    if (auto *t = focusedTerminal()) {
        t->setFocus();
    } else if (auto *t2 = currentTerminal()) {
        t2->setFocus();
    }
}

QSplitter *MainWindow::findParentSplitter(QWidget *w) const {
    if (!w) return nullptr;
    return qobject_cast<QSplitter *>(w->parentWidget());
}

void MainWindow::cleanupEmptySplitters(QWidget *tabRoot) {
    if (!tabRoot) return;

    // Recursively clean up splitters with 0 or 1 children
    auto *splitter = qobject_cast<QSplitter *>(tabRoot);
    if (!splitter) return;

    // First, recurse into children
    for (int i = splitter->count() - 1; i >= 0; --i) {
        auto *childSplitter = qobject_cast<QSplitter *>(splitter->widget(i));
        if (childSplitter) cleanupEmptySplitters(childSplitter);
    }

    if (splitter->count() == 0) {
        // Empty splitter — close the tab
        int idx = m_tabWidget->indexOf(splitter);
        if (idx >= 0) closeTab(idx);
    } else if (splitter->count() == 1) {
        // Only one child left — promote it
        QWidget *child = splitter->widget(0);
        QSplitter *parentSplitter = qobject_cast<QSplitter *>(splitter->parentWidget());

        if (parentSplitter) {
            int idx = parentSplitter->indexOf(splitter);
            child->setParent(nullptr);
            parentSplitter->insertWidget(idx, child);
            splitter->setParent(nullptr);
            splitter->deleteLater();
        } else {
            // This splitter is the tab root
            int tabIdx = m_tabWidget->indexOf(splitter);
            if (tabIdx >= 0) {
                // Transfer session ID from splitter back to the surviving child
                QString sessionId = m_tabSessionIds.value(splitter);
                if (!sessionId.isEmpty()) {
                    m_tabSessionIds.remove(splitter);
                    m_tabSessionIds[child] = sessionId;
                }

                child->setParent(nullptr);
                splitter->setParent(nullptr);
                m_tabWidget->removeTab(tabIdx);
                m_tabWidget->insertTab(tabIdx, child, "Shell");
                m_tabWidget->setCurrentIndex(tabIdx);
                splitter->deleteLater();
            }
        }
    }
}

TerminalWidget *MainWindow::focusedTerminal() const {
    QWidget *focused = QApplication::focusWidget();
    if (auto *t = qobject_cast<TerminalWidget *>(focused))
        return t;
    // Walk up from focused widget
    while (focused) {
        if (auto *t = qobject_cast<TerminalWidget *>(focused))
            return t;
        focused = focused->parentWidget();
    }
    return currentTerminal();
}

// For a given tab root (a TerminalWidget or a QSplitter of panes), return the
// "active" terminal — the descendant that currently holds focus if any, else
// the first one in the subtree. findChild() alone returns an arbitrary first
// child, which gives the wrong pane in split layouts.
static TerminalWidget *activeTerminalInTab(QWidget *root) {
    if (!root) return nullptr;
    if (auto *t = qobject_cast<TerminalWidget *>(root)) return t;
    // Prefer a descendant that currently has focus
    const QList<TerminalWidget *> terms = root->findChildren<TerminalWidget *>();
    for (TerminalWidget *t : terms) {
        if (t && t->hasFocus()) return t;
    }
    return terms.isEmpty() ? nullptr : terms.first();
}

void MainWindow::closeTab(int index) {
    if (m_tabWidget->count() <= 1) {
        close();
        return;
    }

    QWidget *w = m_tabWidget->widget(index);
    if (!w) return;

    // Save info for undo-close-tab — prefer the focused pane for split layouts
    TerminalWidget *term = activeTerminalInTab(w);
    if (term) {
        ClosedTabInfo info;
        info.cwd = term->shellCwd();
        info.title = m_tabWidget->tabText(index);
        m_closedTabs.prepend(info);
        if (m_closedTabs.size() > 10) m_closedTabs.removeLast();
    }

    m_tabSessionIds.remove(w);
    m_tabWidget->removeTab(index);
    w->deleteLater();

    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    if (auto *t = currentTerminal()) {
        t->setFocus();
    }
}

void MainWindow::closeCurrentTab() {
    closeTab(m_tabWidget->currentIndex());
}

void MainWindow::onTabChanged(int /*index*/) {
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (t) {
        t->setFocus();
        onTitleChanged(t->shellTitle());
        // Update Claude integration with the focused terminal's shell
        if (m_claudeIntegration)
            m_claudeIntegration->setShellPid(t->shellPid());
    }
    // Update status bar immediately so CWD/git/process reflect the new tab
    updateStatusBar();
}

TerminalWidget *MainWindow::currentTerminal() const {
    // Prefer the focused pane so split layouts dispatch commands correctly;
    // falls back to the first pane in the tab subtree.
    return activeTerminalInTab(m_tabWidget->currentWidget());
}

void MainWindow::applyTheme(const QString &name) {
    m_currentTheme = name;
    m_config.setTheme(name);

    // Update theme checkmark
    if (m_themeGroup) {
        for (QAction *a : m_themeGroup->actions()) {
            a->setChecked(a->text().remove('&') == name);
        }
    }

    const Theme &theme = Themes::byName(name);

    // UI chrome (title bar, menus, tabs, status bar) always uses opaque backgrounds.
    // Opacity and background alpha only affect the terminal content area — this is
    // handled in TerminalWidget::paintEvent via m_windowOpacity / m_backgroundAlpha.
    QString ss = QString(
        "QMainWindow { background-color: %1; }"
        "QMenuBar { background-color: %2; color: %3; border-bottom: 1px solid %4; }"
        "QMenuBar::item:selected { background-color: %5; border-radius: 4px; }"
        "QMenu { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        "QMenu::item { padding: 6px 24px 6px 12px; border-radius: 4px; }"
        "QMenu::item:selected { background-color: %5; color: %1; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QStatusBar { background-color: %2; color: %6; border-top: 1px solid %4; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar { background-color: %2; }"
        "QTabBar::tab { background-color: %2; color: %6; padding: 6px 16px;"
        "  border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %5; }"
        "QTabBar::tab:hover { background-color: %1; color: %3; }"
        "QTabBar::close-button { image: none; }"
        "QTabBar::close-button:hover { background-color: %7; border-radius: 3px; }"
        "QSplitter::handle { background-color: %4; }"
        "QSplitter::handle:horizontal { width: 2px; }"
        "QSplitter::handle:vertical { height: 2px; }"
        "QWidget#commandPalette { background-color: %2; border: 1px solid %4;"
        "  border-radius: 8px; }"
        "QLineEdit#commandPaletteInput { background: %1; color: %3; border: none;"
        "  border-bottom: 1px solid %4; padding: 10px 14px; font-size: 14px;"
        "  border-radius: 0px; }"
        "QListWidget#commandPaletteList { background: %2; color: %3; border: none;"
        "  outline: none; padding: 4px 0; }"
        "QListWidget#commandPaletteList::item { padding: 6px 14px; }"
        "QListWidget#commandPaletteList::item:selected { background-color: %5; color: %1; }"
    ).arg(theme.bgPrimary.name(),
          theme.bgSecondary.name(),
          theme.textPrimary.name(),
          theme.border.name(),
          theme.accent.name(),
          theme.textSecondary.name(),
          theme.ansi[1].name());  // ANSI red for close/danger

    setStyleSheet(ss);

    m_titleBar->setThemeColors(theme.bgSecondary, theme.textPrimary,
                                theme.accent, theme.border, theme.ansi[1]);

    // Status bar labels use theme colors (null-guarded for first call during construction)
    QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");
    if (m_statusGitBranch)
        m_statusGitBranch->setStyleSheet(QStringLiteral("color: %1; %2").arg(theme.ansi[2].name(), statusStyle));
    if (m_statusMessage)
        m_statusMessage->setStyleSheet(QStringLiteral("color: %1; %2").arg(theme.textPrimary.name(), statusStyle));
    if (m_statusProcess)
        m_statusProcess->setStyleSheet(QStringLiteral("color: %1; %2").arg(theme.ansi[4].name(), statusStyle));

    // Restyle Claude integration widgets
    updateClaudeThemeColors();

    // Apply colors + window opacity to ALL terminal widgets
    double opacity = m_config.opacity();
    QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
    for (auto *t : terminals) {
        t->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor,
                             theme.accent, theme.border);
        t->setWindowOpacityLevel(opacity);
    }

    // Color palette update notification (CSI ? 2031 h) — tell apps the scheme changed
    // 1=dark, 2=light (heuristic: dark themes have bg luminance < 128)
    int scheme = (theme.bgPrimary.lightnessF() < 0.5) ? 1 : 2;
    for (auto *t : terminals) {
        if (t->grid() && t->grid()->colorSchemeNotify()) {
            // Unsolicited report: CSI ? 997 ; scheme n
            t->grid()->sendResponse("\x1B[?997;" + std::to_string(scheme) + "n");
        }
    }

    showStatusMessage(QString("Theme: %1").arg(name), 3000);
}

void MainWindow::moveViaKWin(int targetX, int targetY) {
    qint64 pid = QApplication::applicationPid();
    QString kwinJs = QStringLiteral(
        "var clients = workspace.windowList();\n"
        "for (var i = 0; i < clients.length; i++) {\n"
        "    var c = clients[i];\n"
        "    if (c.pid === %1) {\n"
        "        c.frameGeometry = {\n"
        "            x: %2,\n"
        "            y: %3,\n"
        "            width: c.frameGeometry.width,\n"
        "            height: c.frameGeometry.height\n"
        "        };\n"
        "        break;\n"
        "    }\n"
        "}\n"
    ).arg(pid).arg(targetX).arg(targetY);

    QString scriptPath = QDir::tempPath() + "/kwin_move_ants.js";
    {
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(kwinJs.toUtf8());
    }

    auto *proc = new QProcess(this);
    proc->start("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QStringLiteral("string:%1").arg(scriptPath),
        "string:ants_terminal_move"
    });
    connect(proc, &QProcess::finished, this, [this, proc, scriptPath]() {
        proc->deleteLater();
        auto *proc2 = new QProcess(this);
        proc2->start("dbus-send", {
            "--session", "--dest=org.kde.KWin", "--print-reply",
            "/Scripting", "org.kde.kwin.Scripting.start"
        });
        connect(proc2, &QProcess::finished, this, [proc2, scriptPath]() {
            proc2->deleteLater();
            QProcess::startDetached("dbus-send", {
                "--session", "--dest=org.kde.KWin", "--print-reply",
                "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                "string:ants_terminal_move"
            });
            QFile::remove(scriptPath);
        });
    });
}

void MainWindow::centerWindow() {
    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        int cx = geo.x() + (geo.width() - width()) / 2;
        int cy = geo.y() + (geo.height() - height()) / 2;
        m_posTracker->setPosition(cx, cy);
        m_titleBar->setKnownWindowPos(QPoint(cx, cy));
        m_config.setWindowGeometry(cx, cy, width(), height());
    }

    qint64 pid = QApplication::applicationPid();
    QString kwinJs = QStringLiteral(
        "var clients = workspace.windowList();\n"
        "for (var i = 0; i < clients.length; i++) {\n"
        "    var c = clients[i];\n"
        "    if (c.pid === %1) {\n"
        "        var area = workspace.clientArea(workspace.PlacementArea, c);\n"
        "        c.frameGeometry = {\n"
        "            x: area.x + Math.round((area.width - c.frameGeometry.width) / 2),\n"
        "            y: area.y + Math.round((area.height - c.frameGeometry.height) / 2),\n"
        "            width: c.frameGeometry.width,\n"
        "            height: c.frameGeometry.height\n"
        "        };\n"
        "        break;\n"
        "    }\n"
        "}\n"
    ).arg(pid);

    QString scriptPath = QDir::tempPath() + "/kwin_center_ants.js";
    {
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(kwinJs.toUtf8());
    }

    // Run KWin script asynchronously to avoid blocking the event loop
    auto *proc = new QProcess(this);
    proc->start("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QStringLiteral("string:%1").arg(scriptPath),
        "string:ants_terminal_center"
    });
    connect(proc, &QProcess::finished, this, [this, proc, scriptPath]() {
        proc->deleteLater();
        auto *proc2 = new QProcess(this);
        proc2->start("dbus-send", {
            "--session", "--dest=org.kde.KWin", "--print-reply",
            "/Scripting", "org.kde.kwin.Scripting.start"
        });
        connect(proc2, &QProcess::finished, this, [proc2, scriptPath]() {
            proc2->deleteLater();
            QProcess::startDetached("dbus-send", {
                "--session", "--dest=org.kde.KWin", "--print-reply",
                "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                "string:ants_terminal_center"
            });
            QFile::remove(scriptPath);
        });
    });
}

void MainWindow::toggleMaximize() {
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void MainWindow::changeFontSize(int delta) {
    int size = m_config.fontSize() + delta;
    size = qBound(8, size, 32);
    m_config.setFontSize(size);
    applyFontSizeToAll(size);
}

void MainWindow::applyFontSizeToAll(int size) {
    QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
    for (auto *t : terminals) {
        t->setFontSize(size);
    }
    showStatusMessage(QString("Font size: %1pt").arg(size), 3000);
}

void MainWindow::onTitleChanged(const QString &title) {
    QString windowTitle;
    if (title.isEmpty()) {
        windowTitle = "Ants Terminal";
    } else {
        windowTitle = title + " \u2014 Ants Terminal";
    }
    setWindowTitle(windowTitle);
    m_titleBar->setTitle(windowTitle);
}


void MainWindow::collectActions(QMenu *menu, QList<QAction *> &out) {
    for (QAction *action : menu->actions()) {
        if (action->menu()) {
            // Recurse into submenus, prefix action names
            QString prefix = menu->title().remove('&') + " > ";
            for (QAction *sub : action->menu()->actions()) {
                if (sub->menu()) {
                    // One more level deep
                    QString prefix2 = prefix + action->menu()->title().remove('&') + " > ";
                    for (QAction *sub2 : sub->menu()->actions()) {
                        if (!sub2->isSeparator() && !sub2->text().isEmpty()) {
                            // Create a proxy action with prefixed name
                            auto *proxy = new QAction(prefix2 + sub2->text().remove('&'), this);
                            proxy->setShortcut(sub2->shortcut());
                            connect(proxy, &QAction::triggered, sub2, &QAction::trigger);
                            out.append(proxy);
                        }
                    }
                } else if (!sub->isSeparator() && !sub->text().isEmpty()) {
                    auto *proxy = new QAction(prefix + sub->text().remove('&'), this);
                    proxy->setShortcut(sub->shortcut());
                    connect(proxy, &QAction::triggered, sub, &QAction::trigger);
                    out.append(proxy);
                }
            }
        } else if (!action->isSeparator() && !action->text().isEmpty()) {
            out.append(action);
        }
    }
}

// --- Session persistence ---

void MainWindow::saveAllSessions() {
    if (!m_config.sessionPersistence()) return;
    // Don't overwrite saved sessions if the app ran for less than 5 seconds —
    // this protects against test launches and immediate crashes wiping real data
    if (m_uptimeTimer.elapsed() < 5000) return;

    QStringList tabOrder;
    int activeIndex = 0;
    int currentIdx = m_tabWidget->currentIndex();

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        auto *t = activeTerminalInTab(w);
        if (!t) continue;

        QString tabId = m_tabSessionIds.value(w);
        // For split tabs, the widget (w) is a QSplitter, not the TerminalWidget.
        // Try looking up by the TerminalWidget itself if the tab widget lookup failed.
        if (tabId.isEmpty() && t != w)
            tabId = m_tabSessionIds.value(t);
        if (tabId.isEmpty()) continue;

        // Track active index as position within tabOrder, not the tab widget
        if (i == currentIdx)
            activeIndex = tabOrder.size();

        tabOrder.append(tabId);
        SessionManager::saveSession(tabId, t->grid(), t->shellCwd());
    }
    SessionManager::saveTabOrder(tabOrder, activeIndex);
}

void MainWindow::restoreSessions() {
    if (!m_config.sessionPersistence()) return;

    // Use saved tab order if available, fall back to file modification time
    int activeIndex = 0;
    QStringList sessions = SessionManager::loadTabOrder(&activeIndex);
    if (sessions.isEmpty())
        sessions = SessionManager::savedSessions();
    if (sessions.isEmpty()) return;

    // Close the default empty tab that was created at startup
    bool hadDefaultTab = (m_tabWidget->count() == 1);

    // Collect terminals and their start directories for deferred shell startup
    struct RestoredTab {
        TerminalWidget *terminal;
        QString startDir;
        QString tabId;
    };
    QList<RestoredTab> restoredTabs;

    for (const QString &tabId : sessions) {
        auto *terminal = createTerminal();
        connectTerminal(terminal);

        m_tabWidget->addTab(terminal, "Shell");
        m_tabSessionIds[terminal] = tabId;

        // Restore scrollback, screen, and working directory
        QString savedCwd;
        SessionManager::loadSession(tabId, terminal->grid(), &savedCwd);

        // Restore tab title from saved window title
        QString savedTitle = terminal->grid()->windowTitle();
        if (!savedTitle.isEmpty()) {
            if (savedTitle.length() > 30)
                savedTitle = savedTitle.left(27) + "...";
            m_tabWidget->setTabText(m_tabWidget->count() - 1, savedTitle);
        }

        // Clear screen buffer so new shell starts with a clean display
        // (scrollback history is preserved from the restore above)
        terminal->grid()->clearScreenContent();

        QString startDir;
        if (!savedCwd.isEmpty() && QDir(savedCwd).exists())
            startDir = savedCwd;

        restoredTabs.append({terminal, startDir, tabId});
    }

    // Remove the default empty tab if we restored sessions
    if (hadDefaultTab && m_tabWidget->count() > 1) {
        QWidget *defaultTab = m_tabWidget->widget(0);
        m_tabSessionIds.remove(defaultTab);
        m_tabWidget->removeTab(0);
        defaultTab->deleteLater();
    }

    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    // Defer tab activation and shell startup until after the event loop has
    // processed layout — widgets need their final geometry so that the grid
    // size computed by startShell matches the actual widget size. Without this,
    // non-active tabs get a size mismatch that triggers SIGWINCH, causing bash
    // to redraw its prompt (the "double prompt" bug).
    int idx = std::clamp(activeIndex, 0, m_tabWidget->count() - 1);
    QTimer::singleShot(0, this, [this, restoredTabs, idx]() {
        m_tabWidget->setCurrentIndex(idx);

        for (const auto &tab : restoredTabs) {
            // Recalc grid size now that the widget has its real geometry
            tab.terminal->forceRecalcSize();

            if (!tab.terminal->startShell(tab.startDir))
                continue;

            tab.terminal->update();
            SessionManager::removeSession(tab.tabId);
        }

        if (auto *t = focusedTerminal()) t->setFocus();
    });
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);

    // Center window on first show via KWin scripting.
    // Qt's move()/pos() are broken for frameless windows on KWin compositor,
    // so we always center on open. KWin scripting is the only reliable positioning method.
    if (m_firstShow) {
        m_firstShow = false;
        QTimer::singleShot(150, this, [this]() {
            centerWindow();
        });
    }
}

void MainWindow::updateClaudeThemeColors() {
    const Theme &th = Themes::byName(m_currentTheme);
    QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");
    if (m_claudeStatusLabel)
        m_claudeStatusLabel->setStyleSheet(QStringLiteral("color: %1; %2").arg(th.textSecondary.name(), statusStyle));
    if (m_claudeContextBar)
        m_claudeContextBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: 1px solid %1; border-radius: 3px; background: %2; font-size: 10px; color: %3; }"
                    "QProgressBar::chunk { background: %4; border-radius: 2px; }")
                .arg(th.border.name(), th.bgSecondary.name(), th.textPrimary.name(), th.ansi[2].name()));
    if (m_claudeReviewBtn)
        m_claudeReviewBtn->setStyleSheet(
            QStringLiteral("QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 0 6px; font-size: 10px; }"
                    "QPushButton:hover { background: %3; }")
                .arg(th.bgSecondary.name(), th.textPrimary.name(), th.border.name()));
    if (m_claudeErrorLabel)
        m_claudeErrorLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 4px; font-size: 11px;").arg(th.ansi[1].name()));
}

void MainWindow::setupClaudeIntegration() {
    m_claudeIntegration = new ClaudeIntegration(this);

    // Status bar indicator for Claude Code state
    m_claudeStatusLabel = new QLabel(this);
    m_claudeStatusLabel->setStyleSheet("color: gray; padding: 0 8px; font-size: 11px;");
    m_claudeStatusLabel->hide();
    statusBar()->addPermanentWidget(m_claudeStatusLabel);

    // Context window pressure indicator (progress bar)
    m_claudeContextBar = new QProgressBar(this);
    m_claudeContextBar->setRange(0, 100);
    m_claudeContextBar->setValue(0);
    m_claudeContextBar->setFixedWidth(80);
    m_claudeContextBar->setFixedHeight(14);
    m_claudeContextBar->setFormat("%p%");
    // Styled dynamically by updateClaudeThemeColors()
    m_claudeContextBar->setToolTip("Claude Code context window usage");
    m_claudeContextBar->hide();
    statusBar()->addPermanentWidget(m_claudeContextBar);

    // Review Changes button (shown when Claude edits files)
    m_claudeReviewBtn = new QPushButton("Review Changes", this);
    m_claudeReviewBtn->setFixedHeight(18);
    // Styled dynamically by updateClaudeThemeColors()
    m_claudeReviewBtn->hide();
    statusBar()->addPermanentWidget(m_claudeReviewBtn);
    connect(m_claudeReviewBtn, &QPushButton::clicked, this, &MainWindow::showDiffViewer);

    // Error indicator label
    m_claudeErrorLabel = new QLabel(this);
    // Styled dynamically by updateClaudeThemeColors()
    m_claudeErrorLabel->hide();
    statusBar()->addPermanentWidget(m_claudeErrorLabel);

    connect(m_claudeIntegration, &ClaudeIntegration::stateChanged,
            this, [this](ClaudeState state, const QString &detail) {
        switch (state) {
        case ClaudeState::NotRunning:
            m_claudeStatusLabel->hide();
            m_claudeContextBar->hide();
            break;
        case ClaudeState::Idle: {
            const Theme &th = Themes::byName(m_currentTheme);
            m_claudeStatusLabel->setText("Claude: idle");
            m_claudeStatusLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 8px; font-size: 11px;").arg(th.ansi[2].name()));
            m_claudeStatusLabel->show();
            break;
        }
        case ClaudeState::Thinking: {
            const Theme &th = Themes::byName(m_currentTheme);
            m_claudeStatusLabel->setText("Claude: thinking...");
            m_claudeStatusLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 8px; font-size: 11px;").arg(th.ansi[4].name()));
            m_claudeStatusLabel->show();
            break;
        }
        case ClaudeState::ToolUse: {
            const Theme &th = Themes::byName(m_currentTheme);
            m_claudeStatusLabel->setText(QString("Claude: %1").arg(detail));
            m_claudeStatusLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 8px; font-size: 11px;").arg(th.ansi[3].name()));
            m_claudeStatusLabel->show();
            break;
        }
        }
    });

    connect(m_claudeIntegration, &ClaudeIntegration::contextUpdated,
            this, [this](int percent) {
        // Update context pressure bar
        m_claudeContextBar->setValue(percent);
        m_claudeContextBar->show();
        // Color-code: green < 60%, yellow 60-80%, red > 80%
        const Theme &th = Themes::byName(m_currentTheme);
        QString chunkColor = th.ansi[2].name();  // green
        if (percent > 80) chunkColor = th.ansi[1].name();  // red
        else if (percent > 60) chunkColor = th.ansi[3].name();  // yellow
        m_claudeContextBar->setStyleSheet(
            QStringLiteral("QProgressBar { border: 1px solid %1; border-radius: 3px; background: %2; font-size: 10px; color: %3; }"
                    "QProgressBar::chunk { background: %4; border-radius: 2px; }")
                .arg(th.border.name(), th.bgSecondary.name(), th.textPrimary.name(), chunkColor));
        if (percent >= 80) {
            m_claudeContextBar->setToolTip(
                QString("Context %1% — consider using /compact").arg(percent));
        }
    });

    connect(m_claudeIntegration, &ClaudeIntegration::fileChanged,
            this, [this](const QString &path) {
        showStatusMessage(QString("Claude edited: %1").arg(path), 3000);
        m_claudeReviewBtn->show();
    });

    connect(m_claudeIntegration, &ClaudeIntegration::permissionRequested,
            this, [this](const QString &tool, const QString &input) {
        QString rawRule = tool;
        if (!input.isEmpty()) rawRule += "(" + input + ")";
        // Normalize and generalize to a useful allowlist pattern
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;
        showStatusMessage(QString("Claude permission: %1").arg(rule), 0);

        // Enhanced permission action buttons
        auto *btnWidget = new QWidget(statusBar());
        auto *btnLayout = new QHBoxLayout(btnWidget);
        btnLayout->setContentsMargins(0, 0, 0, 0);
        btnLayout->setSpacing(4);

        const Theme &th = Themes::byName(m_currentTheme);
        auto *allowBtn = new QPushButton("Allow", btnWidget);
        allowBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.ansi[2].name(), th.bgPrimary.name()));
        auto *denyBtn = new QPushButton("Deny", btnWidget);
        denyBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.ansi[1].name(), th.bgPrimary.name()));
        auto *addBtn = new QPushButton("Add to allowlist", btnWidget);
        addBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; border-radius: 3px; padding: 1px 8px; font-size: 10px; }")
            .arg(th.bgSecondary.name(), th.textPrimary.name()));

        btnLayout->addWidget(allowBtn);
        btnLayout->addWidget(denyBtn);
        btnLayout->addWidget(addBtn);
        statusBar()->addPermanentWidget(btnWidget);

        connect(allowBtn, &QPushButton::clicked, btnWidget, [this, btnWidget]() {
            btnWidget->deleteLater();
            clearStatusMessage();
        });
        connect(denyBtn, &QPushButton::clicked, btnWidget, [this, btnWidget]() {
            btnWidget->deleteLater();
            clearStatusMessage();
        });
        connect(addBtn, &QPushButton::clicked, this, [this, rule, btnWidget]() {
            openClaudeAllowlistDialog(rule);
            btnWidget->deleteLater();
            clearStatusMessage();
        });

        // Remove buttons when prompt goes away (new terminal output after grace period)
        auto *term = currentTerminal();
        if (term) {
            QTimer::singleShot(1000, btnWidget, [term, btnWidget]() {
                auto conn = std::make_shared<QMetaObject::Connection>();
                *conn = connect(term, &TerminalWidget::outputReceived, btnWidget, [btnWidget, conn]() {
                    QObject::disconnect(*conn);
                    btnWidget->deleteLater();
                });
            });
        }
    });

    // Set up MCP server with all providers
    QString mcpSocket = QDir::tempPath() + "/ants-terminal-mcp-" +
                        QString::number(QApplication::applicationPid());
    m_claudeIntegration->startMcpServer(mcpSocket);
    m_claudeIntegration->setScrollbackProvider([this](int lines) -> QString {
        if (auto *t = focusedTerminal()) return t->recentOutput(lines);
        return {};
    });
    m_claudeIntegration->setCwdProvider([this]() -> QString {
        if (auto *t = focusedTerminal()) return t->shellCwd();
        return QDir::currentPath();
    });
    m_claudeIntegration->setLastCommandProvider([this]() -> QPair<int,QString> {
        if (auto *t = focusedTerminal())
            return {t->lastExitCode(), t->lastCommandOutput()};
        return {0, {}};
    });
    m_claudeIntegration->setGitStatusProvider([this]() -> QString {
        auto *t = focusedTerminal();
        if (!t) return {};
        QString cwd = t->shellCwd();
        if (cwd.isEmpty()) return {};
        // Run git status + branch + recent log
        QProcess git;
        git.setWorkingDirectory(cwd);
        git.setProgram("git");
        QStringList result;
        // Branch
        git.setArguments({"rev-parse", "--abbrev-ref", "HEAD"});
        git.start(); git.waitForFinished(2000);
        result << "Branch: " + QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        // Status
        git.setArguments({"status", "--porcelain", "-sb"});
        git.start(); git.waitForFinished(2000);
        result << "Status:\n" + QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        // Recent commits
        git.setArguments({"log", "--oneline", "-5"});
        git.start(); git.waitForFinished(2000);
        result << "Recent commits:\n" + QString::fromUtf8(git.readAllStandardOutput()).trimmed();
        return result.join("\n\n");
    });
    m_claudeIntegration->setEnvironmentProvider([this]() -> QString {
        auto *t = focusedTerminal();
        if (!t) return {};
        // Read key env vars from /proc/PID/environ
        pid_t pid = t->shellPid();
        if (pid <= 0) return {};
        QFile envFile(QString("/proc/%1/environ").arg(pid));
        if (!envFile.open(QIODevice::ReadOnly)) return {};
        QByteArray raw = envFile.readAll();
        QStringList vars = QString::fromUtf8(raw).split('\0', Qt::SkipEmptyParts);
        // Filter to useful vars
        QStringList filtered;
        QStringList keys = {"PATH", "VIRTUAL_ENV", "CONDA_DEFAULT_ENV", "NODE_ENV",
                           "SHELL", "EDITOR", "LANG", "HOME", "USER", "TERM", "COLORTERM"};
        for (const QString &v : vars) {
            for (const QString &k : keys) {
                if (v.startsWith(k + "=")) { filtered << v; break; }
            }
        }
        return filtered.join("\n");
    });

    // Start hook server
    m_claudeIntegration->startHookServer();
}

void MainWindow::openClaudeAllowlistDialog(const QString &prefillRule) {
    if (!m_claudeDialog) {
        m_claudeDialog = new ClaudeAllowlistDialog(this);
        connect(m_claudeDialog, &QDialog::finished, this, [this]() {
            if (auto *t = focusedTerminal()) t->setFocus();
        });
    }

    // Resolve .claude/settings.local.json from shell's CWD
    QString cwd;
    if (auto *t = focusedTerminal()) {
        cwd = t->shellCwd();
    }
    if (cwd.isEmpty()) cwd = QDir::currentPath();
    QString settingsPath = cwd + "/.claude/settings.local.json";

    m_claudeDialog->setSettingsPath(settingsPath);
    if (!prefillRule.isEmpty()) {
        m_claudeDialog->prefillRule(prefillRule);
        // Auto-save immediately so Claude Code picks up the rule right away
        m_claudeDialog->saveSettings();
        showStatusMessage("Rule added to allowlist — takes effect on next permission check", 5000);
    }
    m_claudeDialog->show();
    m_claudeDialog->raise();
}

void MainWindow::openClaudeProjectsDialog() {
    if (!m_claudeProjects) {
        m_claudeProjects = new ClaudeProjectsDialog(m_claudeIntegration, &m_config, this);

        // Shell-quote a path (wrap in single quotes, escape existing quotes)
        auto shellQuote = [](const QString &path) -> QString {
            QString escaped = path;
            escaped.replace("'", "'\\''");
            return "'" + escaped + "'";
        };

        // Resume a specific session
        connect(m_claudeProjects, &ClaudeProjectsDialog::resumeSession,
                this, [this, shellQuote](const QString &projectPath, const QString &sessionId, bool fork) {
            auto *t = focusedTerminal();
            if (!t) return;
            QString cmd = QString("cd %1 && claude --resume %2")
                          .arg(shellQuote(projectPath), sessionId);
            if (fork) cmd += " --fork-session";
            t->writeCommand(cmd);
        });

        // Continue the latest session in a project
        connect(m_claudeProjects, &ClaudeProjectsDialog::continueProject,
                this, [this, shellQuote](const QString &projectPath) {
            auto *t = focusedTerminal();
            if (!t) return;
            t->writeCommand(QString("cd %1 && claude --continue").arg(shellQuote(projectPath)));
        });

        // Start a new session in a project
        connect(m_claudeProjects, &ClaudeProjectsDialog::newSession,
                this, [this, shellQuote](const QString &projectPath) {
            auto *t = focusedTerminal();
            if (!t) return;
            t->writeCommand(QString("cd %1 && claude").arg(shellQuote(projectPath)));
        });

        connect(m_claudeProjects, &QDialog::finished, this, [this]() {
            if (auto *t = focusedTerminal()) t->setFocus();
        });
    } else {
        m_claudeProjects->refresh();
    }

    m_claudeProjects->show();
    m_claudeProjects->raise();
}

void MainWindow::moveEvent(QMoveEvent *event) {
    QMainWindow::moveEvent(event);
    m_posTracker->updatePos(event->pos());
}

bool MainWindow::event(QEvent *event) {
    // Detect end of system drag: KWin sends NonClientAreaMouseButtonRelease,
    // or the window gets a MouseButtonRelease, or focus changes.
    if (event->type() == QEvent::NonClientAreaMouseButtonRelease ||
        event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::FocusIn ||
        event->type() == QEvent::WindowActivate) {
        m_titleBar->finishSystemDrag();
    }
    return QMainWindow::event(event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    QPoint realPos = m_posTracker->currentPos();
    m_config.setWindowGeometry(realPos.x(), realPos.y(), width(), height());
    saveAllSessions();
    event->accept();
}

// --- Status bar ---

void MainWindow::showStatusMessage(const QString &msg, int timeoutMs) {
    // Label is created in the constructor before anything that can emit a status
    // message, so a null check here would just mask bugs.
    if (!m_statusMessage) return;
    m_statusMessage->setText(msg);
    if (m_statusMessageTimer) m_statusMessageTimer->stop();
    if (timeoutMs > 0) {
        if (!m_statusMessageTimer) {
            m_statusMessageTimer = new QTimer(this);
            m_statusMessageTimer->setSingleShot(true);
            connect(m_statusMessageTimer, &QTimer::timeout, this, &MainWindow::clearStatusMessage);
        }
        m_statusMessageTimer->start(timeoutMs);
    }
}

void MainWindow::clearStatusMessage() {
    if (m_statusMessage) m_statusMessage->clear();
    if (m_statusMessageTimer) m_statusMessageTimer->stop();
}

void MainWindow::updateStatusBar() {
    if (!m_statusGitBranch || !m_statusProcess)
        return;

    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (!t) return;

    // Git branch (read .git/HEAD). Cached per-cwd for 5 seconds — the poll
    // timer runs every 2s, and walking the directory tree + reading HEAD
    // synchronously can stutter the UI on network mounts or deep trees.
    QString fullCwd = t->shellCwd();
    QString gitBranch;
    if (!fullCwd.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (fullCwd == m_gitCacheCwd && now - m_gitCacheMs < 5000) {
            gitBranch = m_gitCacheBranch;
        } else {
            QString dir = fullCwd;
            while (!dir.isEmpty() && dir != "/") {
                QFile head(dir + "/.git/HEAD");
                if (head.open(QIODevice::ReadOnly)) {
                    QString ref = QString::fromUtf8(head.readAll()).trimmed();
                    if (ref.startsWith("ref: refs/heads/"))
                        gitBranch = ref.mid(16);
                    else if (ref.length() >= 7)
                        gitBranch = ref.left(7); // detached HEAD
                    break;
                }
                int slash = dir.lastIndexOf('/');
                if (slash <= 0) break;
                dir = dir.left(slash);
            }
            m_gitCacheCwd = fullCwd;
            m_gitCacheBranch = gitBranch;
            m_gitCacheMs = now;
        }
    }
    if (!gitBranch.isEmpty()) {
        m_statusGitBranch->setText(" " + gitBranch);
        m_statusGitBranch->show();
    } else {
        m_statusGitBranch->hide();
    }

    // Foreground process
    QString proc = t->foregroundProcess();
    if (!proc.isEmpty()) {
        m_statusProcess->setText(proc);
        m_statusProcess->show();
    } else {
        m_statusProcess->hide();
    }

    // Auto-profile switching (check rules periodically)
    checkAutoProfileRules(t);
}

// --- Tab label customization ---

void MainWindow::updateTabTitles() {
    QString format = m_config.tabTitleFormat();
    if (format == "title") return; // Default shell title behavior, handled by signal

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        auto *t = activeTerminalInTab(w);
        if (!t) continue;

        QString label;
        if (format == "cwd") {
            QString cwd = t->shellCwd();
            if (!cwd.isEmpty()) {
                QFileInfo fi(cwd);
                label = fi.fileName();
            }
        } else if (format == "process") {
            label = t->foregroundProcess();
        } else if (format == "cwd-process") {
            QString cwd = t->shellCwd();
            QString proc = t->foregroundProcess();
            if (!cwd.isEmpty()) {
                QFileInfo fi(cwd);
                label = fi.fileName();
            }
            if (!proc.isEmpty()) {
                if (!label.isEmpty()) label += " - ";
                label += proc;
            }
        }

        if (label.isEmpty()) label = "Shell";
        if (label.length() > 30) label = label.left(27) + "...";
        m_tabWidget->setTabText(i, label);
    }
}

// --- Broadcast input ---
// Handled in connectTerminal via sendToPty forwarding

// --- Quake mode ---

void MainWindow::setupQuakeMode() {
    m_quakeMode = true;
    m_quakeVisible = true;

    // Set window to top of screen, full width
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint | Qt::Tool);
    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        int h = geo.height() / 3;
        resize(geo.width(), h);
        move(geo.x(), geo.y());
    }
    show();
}

void MainWindow::toggleQuakeVisibility() {
    if (!m_quakeMode) return;

    QScreen *screen = this->screen();
    if (!screen) return;
    QRect geo = screen->availableGeometry();
    int h = height();

    if (m_quakeVisible) {
        // Slide up (hide)
        if (!m_quakeAnim) {
            m_quakeAnim = new QPropertyAnimation(this, "pos", this);
            m_quakeAnim->setDuration(200);
            m_quakeAnim->setEasingCurve(QEasingCurve::InQuad);
        }
        m_quakeAnim->stop();
        m_quakeAnim->setStartValue(pos());
        m_quakeAnim->setEndValue(QPoint(geo.x(), geo.y() - h));
        connect(m_quakeAnim, &QPropertyAnimation::finished, this, [this]() {
            hide();
        }, Qt::UniqueConnection);
        m_quakeAnim->start();
        m_quakeVisible = false;
    } else {
        // Slide down (show)
        move(geo.x(), geo.y() - h);
        show();
        raise();
        activateWindow();
        if (!m_quakeAnim) {
            m_quakeAnim = new QPropertyAnimation(this, "pos", this);
            m_quakeAnim->setDuration(200);
            m_quakeAnim->setEasingCurve(QEasingCurve::OutQuad);
        }
        m_quakeAnim->stop();
        m_quakeAnim->setStartValue(QPoint(geo.x(), geo.y() - h));
        m_quakeAnim->setEndValue(QPoint(geo.x(), geo.y()));
        m_quakeAnim->start();
        m_quakeVisible = true;
        if (auto *t = focusedTerminal()) t->setFocus();
    }
}

// --- Trigger handler ---

void MainWindow::onTriggerFired(const QString &pattern, const QString &actionType,
                                 const QString &actionValue) {
    if (actionType == "notify") {
        // Desktop notification via D-Bus
        QString summary = actionValue.isEmpty() ? "Trigger matched" : actionValue;
        QDBusMessage msg = QDBusMessage::createMethodCall(
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "Notify");
        msg << "Ants Terminal"      // app_name
            << uint(0)              // replaces_id
            << ""                   // app_icon
            << QString("Terminal Trigger") // summary
            << QString("Pattern '%1' matched: %2").arg(pattern, summary) // body
            << QStringList()        // actions
            << QVariantMap()        // hints
            << int(5000);           // timeout ms
        QDBusConnection::sessionBus().send(msg);
    } else if (actionType == "sound") {
        QApplication::beep();
    } else if (actionType == "command") {
        if (!actionValue.isEmpty()) {
            QProcess::startDetached("/bin/sh", {"-c", actionValue});
        }
    }
    showStatusMessage(QString("Trigger: '%1' matched").arg(pattern), 3000);
}

// --- Diff Viewer ---

// --- Tab Color Groups ---

void MainWindow::showTabColorMenu(int tabIndex) {
    QMenu menu(this);

    // Capture the tab's QWidget so we resolve the (possibly-shifted) index at
    // action time. Right-click + close-other-tab was renaming / recolouring
    // the wrong tab.
    QWidget *tabWidget = m_tabWidget->widget(tabIndex);
    if (!tabWidget) return;

    // Rename tab
    QAction *renameAction = menu.addAction("Rename Tab...");
    connect(renameAction, &QAction::triggered, this, [this, tabWidget]() {
        int idx = m_tabWidget->indexOf(tabWidget);
        if (idx < 0) return;
        QLineEdit *editor = new QLineEdit(m_tabWidget->tabBar());
        editor->setText(m_tabWidget->tabText(idx));
        editor->selectAll();
        QRect tabRect = m_tabWidget->tabBar()->tabRect(idx);
        editor->setGeometry(tabRect);
        editor->setFocus();
        editor->show();
        connect(editor, &QLineEdit::editingFinished, this, [this, editor, tabWidget]() {
            int curIdx = m_tabWidget->indexOf(tabWidget);
            QString newName = editor->text().trimmed();
            if (curIdx >= 0 && !newName.isEmpty())
                m_tabWidget->setTabText(curIdx, newName);
            editor->deleteLater();
        });
    });

    menu.addSeparator();

    struct ColorEntry { QString name; QColor color; };
    QList<ColorEntry> colors = {
        {"None", QColor()},
        {"Red", QColor(0xF3, 0x8B, 0xA8)},
        {"Green", QColor(0xA6, 0xE3, 0xA1)},
        {"Blue", QColor(0x89, 0xB4, 0xFA)},
        {"Yellow", QColor(0xF9, 0xE2, 0xAF)},
        {"Purple", QColor(0xCB, 0xA6, 0xF7)},
        {"Orange", QColor(0xFA, 0xB3, 0x87)},
        {"Teal", QColor(0x94, 0xE2, 0xD5)},
    };
    for (const auto &ce : colors) {
        QAction *a = menu.addAction(ce.name);
        if (ce.color.isValid()) {
            QPixmap px(12, 12);
            px.fill(ce.color);
            a->setIcon(QIcon(px));
        }
        connect(a, &QAction::triggered, this, [this, tabWidget, ce]() {
            int idx = m_tabWidget->indexOf(tabWidget);
            if (idx < 0) return;
            if (ce.color.isValid()) {
                m_tabColors[idx] = ce.color;
                m_tabWidget->tabBar()->setTabTextColor(idx, ce.color);
            } else {
                m_tabColors.remove(idx);
                m_tabWidget->tabBar()->setTabTextColor(idx, QColor()); // default
            }
        });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::showDiffViewer() {
    auto *t = focusedTerminal();
    if (!t) return;
    QString cwd = t->shellCwd();
    if (cwd.isEmpty()) return;

    // Run git diff to get changes
    QProcess git;
    git.setWorkingDirectory(cwd);
    git.setProgram("git");
    git.setArguments({"diff", "--stat", "--patch"});
    git.start();
    git.waitForFinished(5000);

    QString diff = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
    if (diff.isEmpty()) {
        // Also check staged changes
        git.setArguments({"diff", "--cached", "--stat", "--patch"});
        git.start();
        git.waitForFinished(5000);
        diff = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
    }

    if (diff.isEmpty()) {
        showStatusMessage("No changes detected in git", 3000);
        return;
    }

    // Show in a dialog with syntax coloring
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle("Review Changes");
    dialog->resize(800, 600);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dialog);
    auto *viewer = new QTextEdit(dialog);
    viewer->setReadOnly(true);
    viewer->setFont(QFont("Monospace", 10));

    // Simple diff colorization using theme colors
    const Theme &th = Themes::byName(m_currentTheme);
    QString html = QStringLiteral("<pre style='color: %1; background: %2;'>")
                       .arg(th.textPrimary.name(), th.bgPrimary.name());
    for (const QString &line : diff.split('\n')) {
        QString escaped = line.toHtmlEscaped();
        if (line.startsWith('+') && !line.startsWith("+++"))
            html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[2].name()) + escaped + "</span>\n";
        else if (line.startsWith('-') && !line.startsWith("---"))
            html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[1].name()) + escaped + "</span>\n";
        else if (line.startsWith("@@"))
            html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[4].name()) + escaped + "</span>\n";
        else if (line.startsWith("diff ") || line.startsWith("index "))
            html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[3].name()) + escaped + "</span>\n";
        else
            html += escaped + "\n";
    }
    html += "</pre>";
    viewer->setHtml(html);

    auto *btnBox = new QHBoxLayout;
    auto *closeBtn = new QPushButton("Close", dialog);
    auto *copyBtn = new QPushButton("Copy Diff", dialog);
    btnBox->addStretch();
    btnBox->addWidget(copyBtn);
    btnBox->addWidget(closeBtn);

    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);
    connect(copyBtn, &QPushButton::clicked, this, [diff]() {
        QApplication::clipboard()->setText(diff);
    });

    layout->addWidget(viewer);
    layout->addLayout(btnBox);
    dialog->show();
}

// --- Hot-Reload Configuration ---

void MainWindow::onConfigFileChanged(const QString &path) {
    // Block watcher signals during reload to prevent infinite loop
    // (applyTheme -> setTheme -> save -> triggers watcher -> onConfigFileChanged)
    m_configWatcher->blockSignals(true);

    // Re-add the watch (QFileSystemWatcher drops the watch after some changes)
    if (!m_configWatcher->files().contains(path))
        m_configWatcher->addPath(path);

    // Reload config from disk
    m_config = Config();

    // Re-apply all settings
    applyTheme(m_config.theme());
    applyFontSizeToAll(m_config.fontSize());

    QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
    for (auto *t : terminals) {
        applyConfigToTerminal(t);
        t->setHighlightRules(m_config.highlightRules());
        t->setTriggerRules(m_config.triggerRules());
        QString family = m_config.fontFamily();
        if (!family.isEmpty()) t->setFontFamily(family);
    }

    // Update broadcast
    m_broadcastMode = m_config.broadcastMode();
    if (m_broadcastAction)
        m_broadcastAction->setChecked(m_broadcastMode);

    showStatusMessage("Config reloaded from disk", 3000);
    m_configWatcher->blockSignals(false);
}

// --- Dark/Light Mode Auto-Switching ---

void MainWindow::onSystemColorSchemeChanged() {
    if (!m_config.autoColorScheme()) return;

    Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    QString themeName;
    if (scheme == Qt::ColorScheme::Light)
        themeName = m_config.lightTheme();
    else
        themeName = m_config.darkTheme();

    if (!themeName.isEmpty() && themeName != m_currentTheme) {
        applyTheme(themeName);
        m_config.setTheme(themeName);
        showStatusMessage("Theme auto-switched to " + themeName, 3000);
    }
}

// --- Auto-Profile Switching ---

void MainWindow::checkAutoProfileRules(TerminalWidget *terminal) {
    if (!terminal) return;

    QJsonArray rules = m_config.autoProfileRules();
    if (rules.isEmpty()) return;

    QString cwd = terminal->shellCwd();
    QString title = terminal->shellTitle();
    QString process = terminal->foregroundProcess();

    QJsonObject profiles = m_config.profiles();

    for (const QJsonValue &rv : rules) {
        QJsonObject rule = rv.toObject();
        QString pattern = rule.value("pattern").toString();
        QString type = rule.value("type").toString("title");
        QString profileName = rule.value("profile").toString();

        if (pattern.isEmpty() || profileName.isEmpty()) continue;
        if (!profiles.contains(profileName)) continue;

        QRegularExpression rx(pattern);
        bool matches = false;

        if (type == "title") matches = rx.match(title).hasMatch();
        else if (type == "cwd") matches = rx.match(cwd).hasMatch();
        else if (type == "process") matches = rx.match(process).hasMatch();

        if (matches && profileName != m_lastAutoProfile) {
            m_lastAutoProfile = profileName;

            // Apply the profile settings
            QJsonObject profile = profiles.value(profileName).toObject();
            if (profile.contains("theme")) {
                applyTheme(profile.value("theme").toString());
            }
            if (profile.contains("font_size")) {
                int size = profile.value("font_size").toInt();
                terminal->setFontSize(size);
            }
            if (profile.contains("opacity")) {
                double opacity = profile.value("opacity").toDouble();
                terminal->setWindowOpacityLevel(opacity);
            }
            if (profile.contains("badge_text")) {
                terminal->setBadgeText(profile.value("badge_text").toString());
            }

            showStatusMessage("Profile auto-switched to: " + profileName, 3000);
            return;
        }
    }
}

// --- Command Snippets Dialog ---

void MainWindow::showSnippetsDialog() {
    QDialog *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Command Snippets");
    dialog->setMinimumSize(600, 400);
    dialog->resize(700, 500);

    auto *layout = new QVBoxLayout(dialog);

    // Search bar
    auto *searchEdit = new QLineEdit(dialog);
    searchEdit->setPlaceholderText("Search snippets...");
    layout->addWidget(searchEdit);

    // Snippets list
    auto *table = new QTableWidget(dialog);
    table->setColumnCount(3);
    table->setHorizontalHeaderLabels({"Name", "Command", "Description"});
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(table);

    // Load snippets — heap-allocated so lambdas in the non-modal dialog
    // don't reference a stack variable that goes out of scope
    auto *snippets = new QJsonArray(m_config.snippets());
    connect(dialog, &QObject::destroyed, dialog, [snippets]() { delete snippets; });
    auto loadSnippets = [snippets, table](const QString &filter = "") {
        table->setRowCount(0);
        for (const QJsonValue &sv : *snippets) {
            QJsonObject s = sv.toObject();
            QString name = s.value("name").toString();
            QString cmd = s.value("command").toString();
            QString desc = s.value("description").toString();
            if (!filter.isEmpty() &&
                !name.contains(filter, Qt::CaseInsensitive) &&
                !cmd.contains(filter, Qt::CaseInsensitive) &&
                !desc.contains(filter, Qt::CaseInsensitive))
                continue;
            int row = table->rowCount();
            table->insertRow(row);
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(cmd));
            table->setItem(row, 2, new QTableWidgetItem(desc));
        }
    };
    loadSnippets();

    connect(searchEdit, &QLineEdit::textChanged, dialog, [loadSnippets](const QString &text) {
        loadSnippets(text);
    });

    // Buttons
    auto *btnLayout = new QHBoxLayout();
    auto *addBtn = new QPushButton("Add", dialog);
    auto *editBtn = new QPushButton("Edit", dialog);
    auto *deleteBtn = new QPushButton("Delete", dialog);
    auto *insertBtn = new QPushButton("Insert Command", dialog);
    btnLayout->addWidget(addBtn);
    btnLayout->addWidget(editBtn);
    btnLayout->addWidget(deleteBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(insertBtn);
    layout->addLayout(btnLayout);

    auto editSnippet = [this, snippets, loadSnippets](int editIdx = -1) {
        QDialog editDlg(this);
        editDlg.setWindowTitle(editIdx >= 0 ? "Edit Snippet" : "Add Snippet");
        auto *form = new QFormLayout(&editDlg);

        auto *nameEdit = new QLineEdit(&editDlg);
        auto *cmdEdit = new QLineEdit(&editDlg);
        auto *descEdit = new QLineEdit(&editDlg);

        if (editIdx >= 0 && editIdx < snippets->size()) {
            QJsonObject s = (*snippets)[editIdx].toObject();
            nameEdit->setText(s.value("name").toString());
            cmdEdit->setText(s.value("command").toString());
            descEdit->setText(s.value("description").toString());
        }

        cmdEdit->setPlaceholderText("e.g. docker exec -it {{container}} bash");
        descEdit->setPlaceholderText("Brief description");

        form->addRow("Name:", nameEdit);
        form->addRow("Command:", cmdEdit);
        form->addRow("Description:", descEdit);

        auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(btns);
        connect(btns, &QDialogButtonBox::accepted, &editDlg, &QDialog::accept);
        connect(btns, &QDialogButtonBox::rejected, &editDlg, &QDialog::reject);

        if (editDlg.exec() == QDialog::Accepted) {
            QJsonObject s;
            s["name"] = nameEdit->text();
            s["command"] = cmdEdit->text();
            s["description"] = descEdit->text();

            if (editIdx >= 0)
                (*snippets)[editIdx] = s;
            else
                snippets->append(s);

            m_config.setSnippets(*snippets);
            loadSnippets();
        }
    };

    connect(addBtn, &QPushButton::clicked, dialog, [editSnippet]() { editSnippet(-1); });
    connect(editBtn, &QPushButton::clicked, dialog, [editSnippet, table]() {
        int row = table->currentRow();
        if (row >= 0) editSnippet(row);
    });
    connect(deleteBtn, &QPushButton::clicked, dialog, [snippets, loadSnippets, table, this]() {
        int row = table->currentRow();
        if (row >= 0 && row < snippets->size()) {
            snippets->removeAt(row);
            m_config.setSnippets(*snippets);
            loadSnippets();
        }
    });
    connect(insertBtn, &QPushButton::clicked, dialog, [this, table, snippets, dialog]() {
        int row = table->currentRow();
        if (row < 0 || row >= snippets->size()) return;
        QJsonObject s = (*snippets)[row].toObject();
        QString cmd = s.value("command").toString();

        // Replace {{placeholders}} with user input
        static QRegularExpression placeholderRx("\\{\\{([^}]+)\\}\\}");
        auto it = placeholderRx.globalMatch(cmd);
        QStringList replaced;
        while (it.hasNext()) {
            auto m = it.next();
            QString placeholder = m.captured(1);
            if (replaced.contains(placeholder)) continue;
            QString value = QInputDialog::getText(this, "Parameter: " + placeholder,
                                                   placeholder + ":");
            if (value.isEmpty()) return; // user cancelled
            cmd.replace("{{" + placeholder + "}}", value);
            replaced.append(placeholder);
        }

        if (auto *t = focusedTerminal()) {
            t->writeCommand(cmd);
        }
        dialog->close();
    });

    // Double-click to insert
    connect(table, &QTableWidget::doubleClicked, dialog, [insertBtn]() {
        insertBtn->click();
    });

    dialog->show();
}
