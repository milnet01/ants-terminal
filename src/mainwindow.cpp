#include "mainwindow.h"
#include "terminalwidget.h"
#include "titlebar.h"
#include "commandpalette.h"
#include "aidialog.h"
#include "sshdialog.h"
#include "sessionmanager.h"
#include "xcbpositiontracker.h"

#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#endif

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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Ants Terminal");
    setWindowFlag(Qt::FramelessWindowHint);

    // Restore window size
    resize(m_config.windowWidth(), m_config.windowHeight());

    // Position tracker — bypasses Qt's broken pos()/moveEvent for frameless windows
    m_posTracker = new XcbPositionTracker(this);

    // Apply window opacity from config
    setWindowOpacity(m_config.opacity());

    // Translucent background for compositor transparency
    if (m_config.opacity() < 1.0 || m_config.backgroundAlpha() < 255)
        setAttribute(Qt::WA_TranslucentBackground, true);

    // KDE/KWin background blur
    if (m_config.backgroundBlur())
        setAttribute(Qt::WA_TranslucentBackground, true);

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
        statusBar()->showMessage(msg, 5000);
    });
    connect(m_pluginManager, &PluginManager::logMessage, this, [this](const QString &msg) {
        statusBar()->showMessage("Plugin: " + msg, 3000);
    });
#endif

    // Apply saved theme
    applyTheme(m_config.theme());

    // Create first tab
    newTab();

    // Cleanup old sessions
    SessionManager::cleanupOldSessions(30);
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

    fileMenu->addSeparator();

    QAction *newWindowAction = fileMenu->addAction("&New Window");
    newWindowAction->setShortcut(QKeySequence(m_config.keybinding("new_window", "Ctrl+Shift+N")));
    connect(newWindowAction, &QAction::triggered, this, []() {
        auto *win = new MainWindow();
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
            setWindowOpacity(val);
            setAttribute(Qt::WA_TranslucentBackground,
                         val < 1.0 || m_config.backgroundBlur() || m_config.backgroundAlpha() < 255);
        });
    }

    // Background alpha submenu
    QMenu *alphaMenu = viewMenu->addMenu("Background &Alpha");
    QActionGroup *alphaGroup = new QActionGroup(this);
    alphaGroup->setExclusive(true);
    int currentAlpha = m_config.backgroundAlpha();
    for (int alpha : {255, 230, 204, 178, 153, 128}) {
        int pct = alpha * 100 / 255;
        QAction *a = alphaMenu->addAction(QString("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(alpha == currentAlpha);
        alphaGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, alpha]() {
            m_config.setBackgroundAlpha(alpha);
            QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
            for (auto *t : terminals) t->setBackgroundAlpha(alpha);
            if (alpha < 255)
                setAttribute(Qt::WA_TranslucentBackground, true);
            statusBar()->showMessage(QString("Background alpha: %1%").arg(alpha * 100 / 255), 3000);
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
        statusBar()->showMessage(checked ? "Session logging enabled" : "Session logging disabled", 3000);
    });

    QAction *autoCopyAction = settingsMenu->addAction("&Auto-copy on Select");
    autoCopyAction->setCheckable(true);
    autoCopyAction->setChecked(m_config.autoCopyOnSelect());
    connect(autoCopyAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setAutoCopyOnSelect(checked);
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setAutoCopyOnSelect(checked);
    });

    QAction *blurAction = settingsMenu->addAction("Background &Blur");
    blurAction->setCheckable(true);
    blurAction->setChecked(m_config.backgroundBlur());
    connect(blurAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setBackgroundBlur(checked);
        setAttribute(Qt::WA_TranslucentBackground,
                     checked || m_config.opacity() < 1.0 || m_config.backgroundAlpha() < 255);
        statusBar()->showMessage(checked ? "Blur enabled (restart for full effect)" : "Blur disabled", 3000);
    });

    // GPU rendering toggle
    QAction *gpuAction = settingsMenu->addAction("&GPU Rendering");
    gpuAction->setCheckable(true);
    gpuAction->setChecked(m_config.gpuRendering());
    connect(gpuAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setGpuRendering(checked);
        QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
        for (auto *t : terminals) t->setGpuRendering(checked);
        statusBar()->showMessage(checked ? "GPU rendering enabled" : "GPU rendering disabled", 3000);
    });

    // Session persistence toggle
    QAction *persistAction = settingsMenu->addAction("Session &Persistence");
    persistAction->setCheckable(true);
    persistAction->setChecked(m_config.sessionPersistence());
    connect(persistAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setSessionPersistence(checked);
        statusBar()->showMessage(checked ? "Session persistence enabled" : "Session persistence disabled", 3000);
    });

    settingsMenu->addSeparator();

    QAction *recordAction = settingsMenu->addAction("&Record Session");
    recordAction->setCheckable(true);
    recordAction->setShortcut(QKeySequence(m_config.keybinding("record_session", "Ctrl+Shift+R")));
    connect(recordAction, &QAction::toggled, this, [this, recordAction](bool checked) {
        Q_UNUSED(recordAction);
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
            statusBar()->showMessage("Recording: " + path, 5000);
        } else {
            t->stopRecording();
            statusBar()->showMessage("Recording stopped", 3000);
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
    nextBmAction->setShortcut(QKeySequence(m_config.keybinding("next_bookmark", "Ctrl+Shift+J")));
    connect(nextBmAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->nextBookmark();
    });

    QAction *prevBmAction = settingsMenu->addAction("Previous Bookmark");
    prevBmAction->setShortcut(QKeySequence(m_config.keybinding("prev_bookmark", "Ctrl+Shift+K")));
    connect(prevBmAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->prevBookmark();
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
            statusBar()->showMessage(QString("Scrollback: %1 lines").arg(lines), 3000);
        });
    }

#ifdef ANTS_LUA_PLUGINS
    // Plugins menu
    settingsMenu->addSeparator();
    QAction *reloadPluginsAction = settingsMenu->addAction("Reload &Plugins");
    connect(reloadPluginsAction, &QAction::triggered, this, [this]() {
        m_pluginManager->reloadAll(m_config.enabledPlugins());
        statusBar()->showMessage(
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
    terminal->setBackgroundAlpha(m_config.backgroundAlpha());
    terminal->setGpuRendering(m_config.gpuRendering());
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

    connect(terminal, &TerminalWidget::imagePasted, this, &MainWindow::onImagePasted);
}

void MainWindow::newTab() {
    auto *terminal = createTerminal();
    connectTerminal(terminal);

    QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);
    m_tabSessionIds[terminal] = tabId;

    if (!terminal->startShell()) {
        statusBar()->showMessage("Failed to start shell!");
    }

    terminal->setFocus();

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
        current->setParent(nullptr);
        splitter->addWidget(current);
        splitter->addWidget(newTerm);

        m_tabWidget->removeTab(tabIdx);
        m_tabWidget->insertTab(tabIdx, splitter, "Shell");
        m_tabWidget->setCurrentIndex(tabIdx);
    }

    if (!newTerm->startShell()) {
        statusBar()->showMessage("Failed to start shell!");
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

void MainWindow::closeTab(int index) {
    if (m_tabWidget->count() <= 1) {
        close();
        return;
    }

    QWidget *w = m_tabWidget->widget(index);
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
    }
}

TerminalWidget *MainWindow::currentTerminal() const {
    QWidget *w = m_tabWidget->currentWidget();
    if (!w) return nullptr;
    // If it's directly a TerminalWidget
    if (auto *t = qobject_cast<TerminalWidget *>(w))
        return t;
    // Otherwise find the first TerminalWidget in the widget tree
    return w->findChild<TerminalWidget *>();
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
        "QTabBar::close-button:hover { background-color: #e74856; border-radius: 3px; }"
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
          theme.textSecondary.name());

    setStyleSheet(ss);

    m_titleBar->setThemeColors(theme.bgSecondary, theme.textPrimary,
                                theme.accent, theme.border);

    // Apply colors to ALL terminal widgets (including those in splitters)
    QList<TerminalWidget *> terminals = m_tabWidget->findChildren<TerminalWidget *>();
    for (auto *t : terminals) {
        t->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor,
                             theme.accent, theme.border);
    }

    statusBar()->showMessage(QString("Theme: %1").arg(name), 3000);
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
        connect(proc2, &QProcess::finished, this, [this, proc2, scriptPath]() {
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
        connect(proc2, &QProcess::finished, this, [this, proc2, scriptPath]() {
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
    statusBar()->showMessage(QString("Font size: %1pt").arg(size), 3000);
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

void MainWindow::onShellExited(int code) {
    statusBar()->showMessage(QString("Shell exited with code %1").arg(code));
}

void MainWindow::onImagePasted(const QImage &image) {
    Q_UNUSED(image);
    // Image saving + path insertion is now handled by TerminalWidget
    QString dir = m_config.imagePasteDir();
    if (dir.isEmpty()) dir = QDir::homePath() + "/Pictures/ClaudePaste";
    statusBar()->showMessage(QString("Image pasted -> %1").arg(dir), 5000);
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

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        auto *t = qobject_cast<TerminalWidget *>(w);
        if (!t) t = w->findChild<TerminalWidget *>();
        if (!t) continue;

        QString tabId = m_tabSessionIds.value(w);
        if (tabId.isEmpty()) continue;

        SessionManager::saveSession(tabId, t->grid());
    }
}

void MainWindow::restoreSessions() {
    // Not auto-restoring on startup — expose via menu if needed
}

void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);

    // Center window on first show via KWin scripting.
    // Qt's move()/pos() are broken for frameless windows on KWin compositor,
    // so we always center on open. KWin scripting is the only reliable positioning method.
    static bool firstShow = true;
    if (firstShow) {
        firstShow = false;
        QTimer::singleShot(150, this, [this]() {
            centerWindow();
        });
    }
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
