// ANTS-1677 mainwindow piece 2/8 — the menu bar
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "coloredtabbar.h"
#include "mcporientation.h"  // ANTS-1897 — SessionStart hook installer.
#include "opaquemenubar.h"
#include "secureio.h"          // ANTS-4456 — ensurePrivateDir (0700)
#include "shellutils.h"
#include "themes.h"             // ANTS-1325: include directly where Themes:: is called
#include "terminalwidget.h"
#include "commandpalette.h"
#include "aidialog.h"
#include "sshdialog.h"
#include "settingsdialog.h"
#include "claudeallowlist.h"
#include "claudeintegration.h"
#include "claudeprojects.h"
#include "claudetranscript.h"
#include "aboutdialogs.h"          // ANTS-1181 — About-Ants/About-Qt
#include "auditdialog.h"
#include "coldeyesdialog.h"   // ANTS-1721 — native cold-eyes review dialog.
#include "testauditdialog.h"  // ANTS-1722 — native test-suite review dialog.
#include "indiereviewdialog.h"  // ANTS-1258 — native independent code review.
#include "debuglog.h"
#include "dialogshowtracer.h"
#include "diffviewer.h"           // ANTS-1145 carve-out
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QJsonArray>
#include <QJsonValue>
#include <QFileDialog>
#include <algorithm>
#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#include "luaengine.h"
#endif

using namespace mainwindowdetail;

// ANTS-1181 — setupMenus() was historically a 947-line block stuffed
// inside this same TU. Each top-level menu is now its own helper so the
// menu-bar wiring can be located + read + edited independently of its
// neighbours. setupMenus() is the orchestrator; the helpers contain the
// per-menu body verbatim (no behaviour change).

void MainWindow::setupMenus() {
    setupFileMenu();
    setupEditMenu();
    setupViewMenu();
    setupSplitMenu();
    setupToolsMenu();
    setupSettingsMenu();
    setupHelpMenu();
    setupDonateMenu();

    // One stay-open filter installed on every menu + submenu under the
    // bar. Independent checkboxes stay open on toggle; exclusive radio
    // groups (Themes/Opacity/Scrollback) are exempted inside the filter.
    auto *stayOpen = new StayOpenOnToggleFilter(this);
    for (QMenu *m : m_menuBar->findChildren<QMenu *>())
        m->installEventFilter(stayOpen);
}

void MainWindow::setupFileMenu() {
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
        m_sshDialog->setControlMaster(m_config.sshControlMaster());
        m_sshDialog->show();
        m_sshDialog->raise();
    });

    fileMenu->addSeparator();

    QAction *exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence(m_config.keybinding("exit", "Ctrl+Shift+Q")));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
}

void MainWindow::setupEditMenu() {
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
}

void MainWindow::setupViewMenu() {
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
            // ANTS-3556 — defer the app-wide restyle out of the QMenu's
            // mouse-event / triggered call stack. Applying synchronously here
            // runs qApp->setStyleSheet() (which walks + re-polishes every
            // widget) while the menu is still tearing down a transient
            // status-bar widget (toast / Undo / Claude permission prompt),
            // dereferencing a freed pointer mid-walk → SIGSEGV. applyTheme's
            // own ANTS-2097 activePopupWidget() guard is bypassed because Qt
            // has already dismissed the popup by the time triggered fires
            // (pronounced on Wayland/xdg-popup). singleShot(0) lets the menu
            // fully close + the event stack unwind before the restyle runs.
            QTimer::singleShot(0, this, [this, name]() { applyTheme(name); });
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
            // ANTS-5034 — apply it to the open terminals directly: applyTheme
            // returns early for an unchanged theme, and the config watcher
            // skips our own write.
            for (TerminalWidget *t : liveTerminals())
                t->setWindowOpacityLevel(val);
        });
    }

    // ANTS-5238 — colour every open tab at once, neighbours in different
    // colour families. Persisted exactly like a pick from the tab's menu.
    QAction *distinctColours =
        viewMenu->addAction("Give Each Tab a &Different Colour");
    connect(distinctColours, &QAction::triggered, this,
            &MainWindow::colorTabsDistinctly);

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

    // 0.6.40 — "last completed command" top-level actions. These complement
    // the right-click context menu entries (which operate on the block under
    // the cursor) with keyboard-driven no-selection-needed equivalents, the
    // iTerm2 ⇧⌘O / WezTerm CopyLastOutput convention. Ctrl+Alt+O/R avoid the
    // already-taken Ctrl+Shift+O (split_vertical) / Ctrl+Shift+R (record).
    QAction *copyLastOutputAction = viewMenu->addAction("Copy Last Command &Output");
    copyLastOutputAction->setShortcut(QKeySequence(m_config.keybinding("copy_last_output", "Ctrl+Alt+O")));
    connect(copyLastOutputAction, &QAction::triggered, this, [this]() {
        auto *t = focusedTerminal();
        if (!t) return;
        int n = t->copyLastCommandOutput();
        if (n >= 0) showStatusMessage(QString("Copied %1 chars of last command output").arg(n), 3000);
        else        showStatusMessage("No completed command to copy (enable shell integration)", 3000);
    });

    QAction *rerunLastAction = viewMenu->addAction("Re-run Last Comman&d");
    rerunLastAction->setShortcut(QKeySequence(m_config.keybinding("rerun_last_command", "Ctrl+Alt+R")));
    connect(rerunLastAction, &QAction::triggered, this, [this]() {
        auto *t = focusedTerminal();
        if (!t) return;
        int idx = t->rerunLastCommand();
        if (idx < 0) showStatusMessage("No completed command to re-run (enable shell integration)", 3000);
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
                // ANTS-3556 — same deferral as the initial theme actions
                // above: never restyle synchronously inside the menu's
                // triggered stack (freed-widget SIGSEGV).
                QTimer::singleShot(0, this, [this, name]() { applyTheme(name); });
            });
        }
        showStatusMessage("Themes reloaded", 3000);
    });

    // Performance overlay
    QAction *perfAction = viewMenu->addAction("Performance &Overlay");
    perfAction->setShortcut(QKeySequence("Ctrl+Shift+F12"));
    perfAction->setCheckable(true);
    connect(perfAction, &QAction::toggled, this, [this](bool checked) {
        QList<TerminalWidget *> terminals = liveTerminals();
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
            QList<TerminalWidget *> terminals = liveTerminals();
            for (auto *t : terminals) t->setBackgroundImage("");
            showStatusMessage("Background image cleared", 3000);
        } else {
            m_config.setBackgroundImage(path);
            QList<TerminalWidget *> terminals = liveTerminals();
            for (auto *t : terminals) t->setBackgroundImage(path);
            showStatusMessage("Background image set", 3000);
        }
    });
}

void MainWindow::setupSplitMenu() {
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
}

void MainWindow::setupToolsMenu() {
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
        // Update context and config; ANTS-1168 — reset transient
        // state (input/status/in-flight reply) so a stale "rate
        // limited" surface from a prior open doesn't carry over.
        m_aiDialog->resetTransient();
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
        auto *dlg = new AuditDialog(cwd, this, &m_config);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &AuditDialog::reviewRequested, this, [this](const QString &resultsFile) {
            auto *t = focusedTerminal();
            if (!t) t = currentTerminal();
            if (!t) return;
            // Send a Claude Code command that reads the audit file and fixes issues
            // ANTS-5079 — the whole prompt is shell-quoted, so a `"` or `$(`
            // in the audit results path cannot reach the shell.
            const QString prompt = QString("Read %1 and fix any real issues found in the project audit."
                                  " Focus on bugs, security vulnerabilities, and code quality problems."
                                  " Ignore informational items like line counts and file sizes."
                                  " For each fix, explain what you changed and why.").arg(resultsFile);
            QString cmd = QStringLiteral("claude ") + shellQuote(prompt) + QLatin1Char('\n');
            t->writeCommand(cmd);
        });
        dlg->show();
    });

    // ANTS-1721 / ANTS-1722 — native AI review dialogs (no Claude tokens).
    QMenu *reviewMenu = toolsMenu->addMenu("&Review");
    QAction *coldEyesAction = reviewMenu->addAction("&Cold-eyes Documentation Review...");
    connect(coldEyesAction, &QAction::triggered, this, [this]() {
        QString cwd;
        if (auto *t = focusedTerminal()) cwd = t->shellCwd();
        if (cwd.isEmpty()) cwd = QDir::currentPath();
        auto *dlg = new ColdEyesDialog(cwd, this, &m_config);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    QAction *testAuditAction = reviewMenu->addAction("&Test-suite Audit...");
    connect(testAuditAction, &QAction::triggered, this, [this]() {
        QString cwd;
        if (auto *t = focusedTerminal()) cwd = t->shellCwd();
        if (cwd.isEmpty()) cwd = QDir::currentPath();
        auto *dlg = new TestAuditDialog(cwd, this, &m_config);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    QAction *indieReviewAction = reviewMenu->addAction("&Independent Code Review...");
    connect(indieReviewAction, &QAction::triggered, this, [this]() {
        QString cwd;
        if (auto *t = focusedTerminal()) cwd = t->shellCwd();
        if (cwd.isEmpty()) cwd = QDir::currentPath();
        auto *dlg = new IndieReviewDialog(cwd, this, &m_config);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
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
        // ANTS-1168: scope to focused tab's project so we don't surface
        // a different project's session as "newest by mtime" — calls
        // setProjectFilter which triggers a fresh refresh internally.
        QString projectCwd;
        if (auto *t = focusedTerminal()) projectCwd = t->shellCwd();
        m_claudeTranscript->setProjectFilter(projectCwd);
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

    // ANTS-1187 — user escape hatch when a prior process leaves the
    // scroll region (DECSTBM) constrained, so new output piles in
    // a sub-band of the terminal instead of scrolling against the
    // bottom edge. Most commonly seen with Flask's dev server in
    // a tab that previously hosted a TUI helper. Resets both
    // main + alt scroll regions to full screen without touching
    // grid contents, attrs, modes, or scrollback.
    QAction *resetScrollRegionAction = toolsMenu->addAction(
        "&Reset Scroll Region");
    resetScrollRegionAction->setStatusTip(
        "Clear stuck DECSTBM scroll region — fixes 'output piles "
        "in the middle, bottom rows blank' symptom when a prior "
        "process didn't restore it.");
    connect(resetScrollRegionAction, &QAction::triggered, this, [this]() {
        if (auto *t = focusedTerminal()) {
            t->grid()->resetScrollRegion();
            showStatusMessage(
                QStringLiteral("Scroll region reset to full screen "
                               "[0, %1]").arg(t->grid()->rows() - 1),
                4000);
        }
    });

    toolsMenu->addSeparator();

    // Tools → Debug Mode submenu. Each category is a checkable
    // action; ticking one starts writing that category's events to
    // `~/.local/share/ants-terminal/debug.log`. Bottom of submenu
    // has All / None / Open Log File / Clear Log.
    // ANTS-1863 — restore the persisted debug-category mask so the user's last
    // selection survives a relaunch (the runtime mask otherwise resets to off,
    // losing hook/state logs when resuming a Claude session). ANTS_DEBUG wins:
    // only restore from config when the env var is unset, mirroring the
    // precedence in main.cpp's debug bootstrap. Must run BEFORE the checkable
    // actions below so their initial checked state reflects the restored mask.
    if (!qEnvironmentVariableIsSet("ANTS_DEBUG")) {
        const quint32 savedDebugMask = m_config.debugCategoryMask();
        if (savedDebugMask != 0) DebugLog::setActive(savedDebugMask);
    }
    QMenu *debugMenu = toolsMenu->addMenu("&Debug Mode");
    debugMenu->setToolTipsVisible(true);
    QList<QPair<DebugLog::Category, QString>> catList = {
        {DebugLog::Paint,    "&Paint events (Paint / UpdateRequest / LayoutRequest)"},
        {DebugLog::Events,   "&Events (focus / resize / timer / deferred-delete)"},
        {DebugLog::Input,    "&Input (key / mouse routed to terminal)"},
        {DebugLog::Pty,      "P&TY (reads / writes / resize)"},
        {DebugLog::Vt,       "&VT parser actions"},
        {DebugLog::Render,   "&Render (paint latency, glyph cache)"},
        {DebugLog::Plugins,  "Pl&ugins (Lua event dispatch)"},
        {DebugLog::Network,  "&Network (AI / SSH / git subprocess)"},
        {DebugLog::Config,   "&Config (load / save / change)"},
        {DebugLog::Audit,    "&Audit (tool invocations + findings)"},
        {DebugLog::Claude,   "C&laude Code integration"},
        {DebugLog::Signals,  "&Signal firings"},
        {DebugLog::Shell,    "S&hell integration (OSC 133 / HMAC)"},
        {DebugLog::Session,  "Sessi&on persistence"},
        {DebugLog::Perf,     "Per&f (event-loop stalls, slow handlers)"},
    };
    for (const auto &entry : catList) {
        QAction *a = debugMenu->addAction(entry.second);
        a->setCheckable(true);
        a->setChecked((DebugLog::active() & entry.first) != 0);
        const quint32 bit = entry.first;
        connect(a, &QAction::toggled, this, [this, bit](bool on) {
            quint32 cur = DebugLog::active();
            if (on) cur |= bit; else cur &= ~bit;
            DebugLog::setActive(cur);
            m_config.setDebugCategoryMask(cur);  // ANTS-1863 persist
        });
    }
    debugMenu->addSeparator();
    QAction *debugAllAction = debugMenu->addAction("Enable &All Categories");
    connect(debugAllAction, &QAction::triggered, this, [this, debugMenu]() {
        DebugLog::setActive(DebugLog::All);
        m_config.setDebugCategoryMask(DebugLog::active());  // ANTS-1863 persist
        for (QAction *a : debugMenu->actions())
            if (a->isCheckable()) a->setChecked(true);
    });
    QAction *debugNoneAction = debugMenu->addAction("Disable All (&Off)");
    connect(debugNoneAction, &QAction::triggered, this, [this, debugMenu]() {
        DebugLog::setActive(DebugLog::None);
        m_config.setDebugCategoryMask(0);  // ANTS-1863 persist
        for (QAction *a : debugMenu->actions())
            if (a->isCheckable()) a->setChecked(false);
    });
    debugMenu->addSeparator();
    // 0.7.58 (ANTS-1054 follow-up) — runtime toggle for the dialog
    // spawn tracer. Same entry point as the ANTS_TRACE_DIALOGS=1 env
    // var path; ticking starts logging top-level QWidget/QDialog show
    // events to stderr (and to the debug log when Events category is
    // also active). Useful for capturing the "mystery flashing
    // dialog" without restarting.
    QAction *debugTraceDialogsAction = debugMenu->addAction(
        "&Trace dialog show events (writes to stderr)");
    debugTraceDialogsAction->setCheckable(true);
    debugTraceDialogsAction->setChecked(DialogShowTracer::active());
    connect(debugTraceDialogsAction, &QAction::toggled, this,
            [this](bool on) {
        DialogShowTracer::setActive(on);
        showStatusMessage(on
            ? QStringLiteral("Dialog-show tracer enabled — events logging "
                             "to stderr (see debug log if running attached)")
            : QStringLiteral("Dialog-show tracer disabled"),
            6000);
    });
    debugMenu->addSeparator();
    QAction *debugOpenAction = debugMenu->addAction("Open &Log File");
    connect(debugOpenAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(DebugLog::logFilePath()));
    });
    QAction *debugClearAction = debugMenu->addAction("&Clear Log File");
    connect(debugClearAction, &QAction::triggered, this, [this]() {
        DebugLog::clear();
        showStatusMessage(QStringLiteral("Debug log cleared: %1")
                            .arg(DebugLog::logFilePath()), 4000);
    });
}

void MainWindow::setupSettingsMenu() {
    QMenu *settingsMenu = m_menuBar->addMenu("S&ettings");

    QAction *loggingAction = settingsMenu->addAction("Session &Logging");
    loggingAction->setCheckable(true);
    loggingAction->setChecked(m_config.sessionLogging());
    connect(loggingAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setSessionLogging(checked);
        // Apply to all terminals
        QList<TerminalWidget *> terminals = liveTerminals();
        // ANTS-5151 — a terminal that refused (its log could not be made
        // private) has already put its reason in the status bar; do not
        // overwrite it with a success message.
        bool refused = false;
        for (auto *t : terminals) {
            t->setSessionLogging(checked);
            if (checked && !t->sessionLogging()) refused = true;
        }
        if (!refused)
            showStatusMessage(checked ? "Session logging enabled" : "Session logging disabled", 3000);
    });

    QAction *autoCopyAction = settingsMenu->addAction("&Auto-copy on Select");
    autoCopyAction->setCheckable(true);
    autoCopyAction->setChecked(m_config.autoCopyOnSelect());
    connect(autoCopyAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setAutoCopyOnSelect(checked);
        QList<TerminalWidget *> terminals = liveTerminals();
        for (auto *t : terminals) t->setAutoCopyOnSelect(checked);
    });

    QAction *bellAction = settingsMenu->addAction("&Visual Bell");
    bellAction->setCheckable(true);
    bellAction->setChecked(m_config.visualBell());
    connect(bellAction, &QAction::toggled, this, [this](bool checked) {
        m_config.setVisualBell(checked);
        QList<TerminalWidget *> terminals = liveTerminals();
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
    connect(recordAction, &QAction::toggled, this, [this, recordAction](bool checked) {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (!t) return;
        if (checked) {
            QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                          + "/ants-terminal/recordings";
            // ANTS-4456 — a recording is the terminal's whole byte stream, so
            // its directory is born 0700 rather than created at umask. Signals
            // are blocked while un-checking: setChecked re-enters this lambda
            // on the false arm, which would overwrite the message below with
            // "Recording stopped".
            if (!ensurePrivateDir(dir)) {
                const QSignalBlocker block(recordAction);
                recordAction->setChecked(false);
                showStatusMessage("Could not secure the recordings directory "
                                  "— not recording", 5000);
                return;
            }
            QString path = dir + "/recording_"
                + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".cast";
            t->startRecording(path);
            if (!t->isRecording()) {
                // ANTS-5151 — refused: the recording file could not be made
                // private, and the terminal has put the reason in the status
                // bar. Un-check with signals blocked, as above.
                const QSignalBlocker block(recordAction);
                recordAction->setChecked(false);
                return;
            }
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

    // ANTS-1165: previous defaults of Ctrl+Shift+Down / Ctrl+Shift+Up
    // collided with the OSC 133 prompt-navigation chord that
    // TerminalWidget::keyPressEvent intercepts before Qt dispatches
    // QShortcuts (see view-menu comment near setupPromptNav). The
    // bookmark shortcut was therefore silently dead whenever the
    // terminal had focus. Move both defaults to Ctrl+Alt+Up/Down,
    // which TerminalWidget does not intercept.
    QAction *nextBmAction = settingsMenu->addAction("Next Bookmark");
    nextBmAction->setShortcut(QKeySequence(m_config.keybinding("next_bookmark", "Ctrl+Alt+Down")));
    connect(nextBmAction, &QAction::triggered, this, [this]() {
        TerminalWidget *t = focusedTerminal();
        if (!t) t = currentTerminal();
        if (t) t->nextBookmark();
    });

    QAction *prevBmAction = settingsMenu->addAction("Previous Bookmark");
    prevBmAction->setShortcut(QKeySequence(m_config.keybinding("prev_bookmark", "Ctrl+Alt+Up")));
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
            QList<TerminalWidget *> terminals = liveTerminals();
            for (auto *t : terminals) t->setMaxScrollback(lines);
            showStatusMessage(QString("Scrollback: %1 lines").arg(lines), 3000);
        });
    }

    settingsMenu->addSeparator();

    // Broadcast input toggle. ANTS-5223 — reaches the panes of the tab being
    // typed in, starts off on every launch (the choice is never saved), and
    // shows a status-bar chip while on.
    m_broadcastAction = settingsMenu->addAction("&Broadcast Input to Panes in This Tab");
    m_broadcastAction->setCheckable(true);
    m_broadcastAction->setShortcut(QKeySequence(m_config.keybinding("broadcast_input", "Ctrl+Shift+I")));
    connect(m_broadcastAction, &QAction::toggled, this, [this](bool checked) {
        m_broadcastMode = checked;
        refreshBroadcastChip();
        showStatusMessage(checked ? "Broadcast ON — typing goes to every pane in this tab"
                                  : "Broadcast OFF", 3000);
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
        // ANTS-5078 — the export writes in slices and fails through
        // captureFailed; success is announced once it has been written.
        const auto format = path.endsWith(".html", Qt::CaseInsensitive)
                                ? ScrollbackExporter::Format::Html
                                : ScrollbackExporter::Format::Text;
        if (!t->startExport({.format = format, .path = path})) return;
        connect(t, &TerminalWidget::exportFinished, this,
                [this](const QString &written, bool ok) {
                    if (ok) showStatusMessage("Scrollback exported to " + written, 5000);
                }, Qt::SingleShotConnection);
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
                if (m_claudeIntegration)
                    m_claudeIntegration->setContextWindowTokens(
                        m_config.claudeContextWindowTokens());
                applyFontSizeToAll(m_config.fontSize());

                QList<TerminalWidget *> terminals = liveTerminals();
                for (auto *t : terminals) {
                    applyConfigToTerminal(t);
                    t->setHighlightRules(m_config.highlightRules());
                    t->setTriggerRules(m_config.triggerRules());
                    QString family = m_config.fontFamily();
                    if (!family.isEmpty()) t->setFontFamily(family);
                }

                // Opacity is now applied via per-pixel alpha in applyTheme() above

                // Update quake mode. Wire the hotkey too — pre-ANTS-1738
                // this site called only setupQuakeMode(), so enabling Quake
                // via Preferences gave a drop-down with no working hotkey
                // until the next restart.
                if (m_config.quakeMode() && !m_quakeMode) {
                    setupQuakeMode();
                    wireQuakeHotkey();
                }

#ifdef ANTS_LUA_PLUGINS
                // 0.6.9 — let plugins react to settings changes (re-read their
                // own settings, refresh status text, etc.). Payload is empty
                // because the relevant config bits are accessed via
                // ants.settings.get on demand.
                if (m_pluginManager)
                    m_pluginManager->fireEvent(PluginEvent::WindowConfigReloaded, QString());
#endif

                // ANTS-1901 — propagate the master MCP toggle live. Off is
                // honoured immediately: the dispatcher refuses every verb
                // (setMcpEnabled) and the orientation hook is removed so
                // future Claude sessions don't show the cheat-sheet. The
                // socket itself is bound/unbound only at launch (see
                // setupStatusBarChrome), so turning the switch back ON takes
                // effect on the next start.
                if (m_claudeIntegration)
                    m_claudeIntegration->setMcpEnabled(m_config.claudeMcpEnabled());
                if (!m_config.claudeMcpEnabled())
                    ants::mcp_orientation::uninstall();

                showStatusMessage("Settings applied", 3000);
            });
            connect(m_settingsDialog, &QDialog::finished, this, [this]() {
                if (auto *t = focusedTerminal()) t->setFocus();
            });
        }
        // Hand off the current plugin snapshot each time the dialog opens so
        // hot-reloads / new installs are reflected in the capability-audit
        // tab without needing to recreate the dialog. When plugins are
        // compiled out the list is empty and the tab shows a guidance note.
        QList<SettingsDialog::PluginDisplay> pluginDisplays;
#ifdef ANTS_LUA_PLUGINS
        if (m_pluginManager) {
            for (const auto &info : m_pluginManager->plugins()) {
                SettingsDialog::PluginDisplay d;
                d.name = info.name;
                d.version = info.version;
                d.description = info.description;
                d.author = info.author;
                d.permissions = info.permissions;
                pluginDisplays << d;
            }
        }
#endif
        m_settingsDialog->setPlugins(pluginDisplays);
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

void MainWindow::setupHelpMenu() {
    // Standard last-position menu carrying About (user-requested 2026-04-24
    // — there was no GUI-surfaced way to check the running version before;
    // `ants-terminal --version` on the CLI was the only path). About Qt
    // uses Qt's stock dialog so we inherit future Qt-version bumps
    // automatically. Our About shows the app version (ANTS_VERSION, single
    // source of truth in CMakeLists.txt), the Qt runtime version, the Lua
    // engine version when compiled in, and the homepage URL.
    QMenu *helpMenu = m_menuBar->addMenu("&Help");

    QAction *aboutAction = helpMenu->addAction("&About Ants Terminal...");
    connect(aboutAction, &QAction::triggered, this, [this]() {
        // ANTS-5341 — each tab's shells, so the dialog can name a tab whose
        // Claude Code session runs an older ants-mcpd. A split tab has several.
        QList<AboutDialogs::TabShells> tabs;
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            QWidget *root = m_tabWidget->widget(i);
            AboutDialogs::TabShells tab;
            tab.title = m_tabWidget->tabText(i);
            QList<TerminalWidget *> terms = root->findChildren<TerminalWidget *>();
            if (auto *self = qobject_cast<TerminalWidget *>(root)) terms.append(self);
            for (TerminalWidget *t : terms)
                if (t->shellPid() > 0) tab.shellPids.append(t->shellPid());
            tabs.append(tab);
        }
        AboutDialogs::showAboutAnts(this, tabs);
    });

    QAction *aboutQtAction = helpMenu->addAction("About &Qt...");
    connect(aboutQtAction, &QAction::triggered, this, [this]() {
        AboutDialogs::showAboutQt(this);
    });

    // ANTS-5558 — reopen the welcome dialog at any time, key or no key.
    QAction *welcomeAction = helpMenu->addAction(tr("Show &Welcome..."));
    welcomeAction->setObjectName(QStringLiteral("helpShowWelcomeAction"));
    connect(welcomeAction, &QAction::triggered, this, &MainWindow::showWelcome);

    helpMenu->addSeparator();
    // 0.7.47 — manual update check. The startup probe already runs
    // 5 s after launch (see m_updateAvailableAction wiring); this
    // gives the user a way to re-check on demand without restarting.
    QAction *checkUpdatesAction = helpMenu->addAction(tr("Check for &Updates"));
    checkUpdatesAction->setObjectName(
        QStringLiteral("helpCheckForUpdatesAction"));
    connect(checkUpdatesAction, &QAction::triggered, this, [this]() {
        showStatusMessage(tr("Checking for updates…"), 2000);
        checkForUpdates(/*userInitiated=*/true);
    });
}

void MainWindow::setupDonateMenu() {
    // User-requested 2026-06-30 — surface the project's funding pages in
    // the GUI so supporters don't have to hunt for them on GitHub. Both
    // actions open the user's default browser via QDesktopServices (the
    // Qt 6 idiom — dispatches to xdg-open / ShellExecute / `open`). URLs
    // are stable funding landing pages; the GitHub Sponsors handle mirrors
    // .github/FUNDING.yml (`github: [milnet01]`). Deliberately wired LAST
    // (after setupHelpMenu) so Donate is the rightmost menu — a
    // call-to-action the user wants maximally visible, which is why it
    // overrides the freedesktop "Help is last" convention
    // (help_about_menu spec, Invariant 1).
    QMenu *donateMenu = m_menuBar->addMenu(tr("&Donate"));

    QAction *githubAction = donateMenu->addAction(tr("Sponsor on &GitHub..."));
    connect(githubAction, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://github.com/sponsors/milnet01")));
        showStatusMessage(tr("Opening GitHub Sponsors in your browser…"), 3000);
    });

    QAction *patreonAction = donateMenu->addAction(tr("Support on &Patreon..."));
    connect(patreonAction, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://www.patreon.com/c/AntsProjectsHub")));
        showStatusMessage(tr("Opening Patreon in your browser…"), 3000);
    });

    // ANTS-5558 — the one-off tip link from .github/FUNDING.yml.
    QAction *paybruAction = donateMenu->addAction(tr("Tip via &PayBru..."));
    connect(paybruAction, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://paybru.co.za/tip/ants-projects-hub")));
        showStatusMessage(tr("Opening PayBru in your browser…"), 3000);
    });
}
