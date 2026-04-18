#include "mainwindow.h"

#include "coloredtabbar.h"
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
#include "elidedlabel.h"

#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#endif

#include <algorithm>
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QMoveEvent>
#include <QMenuBar>
#include <QFrame>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
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
#include <QPainter>
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

    // Tab widget with custom ColoredTabBar so per-tab colour groups
    // render independently of the QTabBar::tab { color: … } stylesheet
    // rule (which would otherwise pre-empt any setTabTextColor call
    // and silently suppress the user's chosen colour).
    m_tabWidget = new ColoredTabWidget(this);
    m_coloredTabBar = m_tabWidget->coloredTabBar();
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Tab bar context menu for tab groups (color labels)
    m_coloredTabBar->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_coloredTabBar, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        int tabIdx = m_coloredTabBar->tabAt(pos);
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
    rebuildCommandPalette();

#ifdef ANTS_LUA_PLUGINS
    // Initialize plugin system
    QString pluginDir = m_config.pluginDir();
    if (pluginDir.isEmpty()) {
        pluginDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                    + "/ants-terminal/plugins";
    }
    m_pluginManager = new PluginManager(this);
    m_pluginManager->setPluginDir(pluginDir);

    // Persist + retrieve manifest v2 grants via Config
    m_pluginManager->setGrantStore(
        [this](const QString &name) { return m_config.pluginGrants(name); },
        [this](const QString &name, const QStringList &grants) {
            m_config.setPluginGrants(name, grants);
        });

    // Permission prompt: dialog listing requested permissions with Accept/Deny.
    // Users get the browser-extension UX — explicit opt-in for each permission.
    m_pluginManager->setPermissionPrompt(
        [this](const PluginInfo &info, const QStringList &requested) -> QStringList {
            QDialog dlg(this);
            dlg.setWindowTitle(QString("Plugin permissions: %1").arg(info.name));
            auto *layout = new QVBoxLayout(&dlg);
            auto *label = new QLabel(QString(
                "The plugin <b>%1</b> (v%2) is requesting the following "
                "permissions. Uncheck any you don't want to grant.")
                .arg(info.name, info.version), &dlg);
            label->setWordWrap(true);
            layout->addWidget(label);
            QList<QCheckBox *> boxes;
            for (const QString &p : requested) {
                auto *cb = new QCheckBox(p, &dlg);
                cb->setChecked(true);
                // Permission descriptions
                QString tip = p;
                if (p == "clipboard.write") tip = "Write to the system clipboard.";
                else if (p == "settings")   tip = "Store key/value settings under the plugin's name.";
                else if (p == "net")        tip = "Reserved for future use (network access).";
                cb->setToolTip(tip);
                layout->addWidget(cb);
                boxes << cb;
            }
            auto *btns = new QDialogButtonBox(
                QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            btns->button(QDialogButtonBox::Ok)->setText("Accept");
            btns->button(QDialogButtonBox::Cancel)->setText("Deny all");
            connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            layout->addWidget(btns);
            QStringList out;
            if (dlg.exec() == QDialog::Accepted) {
                for (int i = 0; i < boxes.size(); ++i) {
                    if (boxes[i]->isChecked()) out << requested[i];
                }
            }
            return out;
        });

    m_pluginManager->scanAndLoad(m_config.enabledPlugins());

    // Manifest v2: register plugin keybindings. Rescan on pluginsReloaded so
    // hot-reload picks up newly-added or changed shortcuts without restart.
    auto registerPluginKeybindings = [this]() {
        // Drop any previously-registered plugin shortcuts
        for (auto *sc : m_pluginShortcuts) sc->deleteLater();
        m_pluginShortcuts.clear();
        for (const auto &info : m_pluginManager->plugins()) {
            if (!info.enabled) continue;
            const QJsonObject &kb = info.keybindings;
            for (auto it = kb.constBegin(); it != kb.constEnd(); ++it) {
                QString actionId = it.key();
                QString seq = it.value().toString();
                if (seq.isEmpty()) continue;
                QKeySequence ks(seq);
                if (ks.isEmpty()) {
                    showStatusMessage(QString("Plugin %1: invalid keybinding '%2' for '%3'")
                                       .arg(info.name, seq, actionId), 6000);
                    continue;
                }
                auto *sc = new QShortcut(ks, this);
                QString pluginName = info.name;
                connect(sc, &QShortcut::activated, this, [this, pluginName, actionId]() {
                    // Route to the plugin's keybinding handler; payload = actionId
                    if (auto *engine = m_pluginManager->engineFor(pluginName)) {
                        engine->fireEvent(PluginEvent::Keybinding, actionId);
                    }
                });
                m_pluginShortcuts.append(sc);
            }
        }
    };
    registerPluginKeybindings();
    connect(m_pluginManager, &PluginManager::pluginsReloaded, this, registerPluginKeybindings);
    connect(m_pluginManager, &PluginManager::sendToTerminal, this, [this](const QString &text) {
        if (auto *t = focusedTerminal()) t->writeCommand(text);
    });
    connect(m_pluginManager, &PluginManager::statusMessage, this, [this](const QString &msg) {
        showStatusMessage(msg, 5000);
    });
    connect(m_pluginManager, &PluginManager::logMessage, this, [this](const QString &msg) {
        showStatusMessage("Plugin: " + msg, 3000);
    });
    // ants.clipboard.write — capability-gated clipboard write
    connect(m_pluginManager, &PluginManager::clipboardWriteRequested, this,
            [](const QString &text) { QApplication::clipboard()->setText(text); });
    // ants.settings.get / set — backed by Config::pluginSetting[s]
    connect(m_pluginManager, &PluginManager::settingsGetRequested, this,
            [this](const QString &pluginName, const QString &key, QString &out) {
                out = m_config.pluginSetting(pluginName, key);
            });
    connect(m_pluginManager, &PluginManager::settingsSetRequested, this,
            [this](const QString &pluginName, const QString &key, const QString &value) {
                m_config.setPluginSetting(pluginName, key, value);
            });
    // 0.6.9 — palette entries from ants.palette.register({...}). Each call
    // appends one entry and rebuilds the Ctrl+Shift+P list. Hot reload
    // discards all entries first (via pluginsReloaded below) so stale
    // entries from removed plugins don't survive across reloads.
    connect(m_pluginManager, &PluginManager::paletteEntryRegistered, this,
            &MainWindow::onPluginPaletteRegistered);
    // Drop all plugin palette entries on a full reload — init.lua re-runs
    // and re-registers anything that should still be there. Without this
    // each reload would double-register every entry.
    connect(m_pluginManager, &PluginManager::pluginsReloaded, this, [this]() {
        // Tear down all plugin entries; they'll be re-added by re-running
        // init.lua during scanAndLoad.
        for (auto &e : m_pluginPaletteEntries) {
            if (e.qaction)  e.qaction->deleteLater();
            if (e.shortcut) e.shortcut->deleteLater();
        }
        m_pluginPaletteEntries.clear();
        rebuildCommandPalette();
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

    // Apply ordered tab-color sequence from the previous run. Must run
    // AFTER all tabs are in place. For session-persistence ON, the
    // UUID-keyed path inside restoreSessions already colored matching
    // tabs; this call leaves those alone (see applyTabColorSequence's
    // "already colored" guard) and only paints the uncolored slots.
    // For session-persistence OFF, this is the ONLY path that colors
    // tabs on startup — UUIDs won't match so tab_groups looked empty.
    applyTabColorSequence();

    // Status bar info widgets (git branch, status message, process).
    // Transient status messages go into m_statusMessage (not statusBar()->showMessage),
    // so the git branch label stays visible to the left of them.
    //
    // 0.6.26 — pin the bar to a consistent minimum height. User report:
    // status bar's height jumped when the transient "Add to allowlist"
    // button appeared (tall, inherits global QPushButton padding) and
    // shrank when it disappeared, leaving only label-height widgets.
    // QStatusBar's size hint is max(child size hints); without a floor,
    // it follows the tallest child. Pinning a floor that covers the
    // default button height keeps the bar visually stable as children
    // come and go. Value chosen to match global QPushButton: text height
    // (~14px at the app font) + padding 6px·2 + border 1px·2 ≈ 28–30px,
    // plus a small QStatusBar internal margin → 32px is comfortable.
    statusBar()->setMinimumHeight(32);

    // Status-bar layout rule (user feedback 2026-04-18): the git branch,
    // process name, Claude status, and transient buttons are FIXED-width —
    // their sizeHint is their natural width, QSizePolicy::Fixed prevents
    // QStatusBar's internal QBoxLayout from squeezing them. The ONLY
    // elastic widget is m_statusMessage (stretch=1, ElideMiddle); when
    // the bar runs out of space it is the notification that gets
    // truncated with "…", never the informational chips. Past
    // regressions where the branch label rendered as "…" were all
    // traced to ElidedLabel + stylesheet padding miscalculation under
    // layout pressure; plain QLabel + Fixed sizePolicy sidesteps the
    // entire class of bug.
    m_statusGitBranch = new QLabel(this);
    m_statusGitBranch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_statusGitBranch->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(m_statusGitBranch);

    // 0.6.26 — the "chip" styling on the branch label (rounded bg + border)
    // blends into the status bar background on low-contrast themes (Gruvbox
    // especially). A hard QFrame::VLine between the branch label and the
    // transient-status slot gives a deterministic divider that survives
    // every theme. Cheap widget, painted from the theme's border color via
    // the global QFrame stylesheet / palette.
    m_statusGitSep = new QFrame(this);
    m_statusGitSep->setFrameShape(QFrame::VLine);
    m_statusGitSep->setFrameShadow(QFrame::Plain);
    m_statusGitSep->setFixedWidth(1);
    m_statusGitSep->setContentsMargins(0, 4, 0, 4);
    m_statusGitSep->hide();  // shown whenever the branch label is shown
    statusBar()->addWidget(m_statusGitSep);

    {
        // Middle slot (stretch=1) — elide-middle keeps both the leading
        // label ("Claude permission:") and the trailing detail visible
        // when the combined string overflows available width.
        auto *lbl = new ElidedLabel(this);
        lbl->setElideMode(Qt::ElideMiddle);
        m_statusMessage = lbl;
    }
    statusBar()->addWidget(m_statusMessage, 1);

    m_statusProcess = new QLabel(this);
    m_statusProcess->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    statusBar()->addWidget(m_statusProcess);

    // Status update timer (every 2 seconds). updateStatusBar() walks the
    // terminal's cwd for .git/HEAD (for the branch label) and the
    // /proc/PID/comm (for the foreground-process label). refreshReviewButton()
    // spawns a non-blocking `git diff --quiet HEAD` — both cheap, both
    // async. Coupling both to the same tick ensures the Review Changes
    // button reflects git state changes the user made outside of Claude's
    // hooks (manual `git add`, edits from another editor, etc.) without
    // waiting for a tab-switch. Previously refreshReviewButton was tied
    // only to tab-switch + hook fileChanged, which left the button hidden
    // on boot in a dirty repo and during hookless workflows.
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateTabTitles);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshReviewButton);
    m_statusTimer->start();

    // Populate the status bar immediately so the user sees correct state
    // on boot instead of waiting 2 s for the first timer tick. onTabChanged
    // fires during initial addTab() above but *before* the status widgets
    // here were created, so those updates were no-ops (guarded against
    // null m_statusGitBranch). This is the first call after the widgets
    // exist.
    QTimer::singleShot(0, this, [this]() {
        updateStatusBar();
        refreshReviewButton();
    });

    // 0.6.26 — auto-return focus to the active terminal whenever focus
    // lands on "chrome" widgets (status bar buttons, tab bar, menu bar
    // leftovers) without an active dialog. User report: "If there is no
    // window open, please always ensure focus is set to the terminal
    // prompt." Clicking a status bar button (Review Changes, Add to
    // allowlist, etc.) previously left keyboard focus parked on the
    // button or status bar, so subsequent keystrokes didn't reach the
    // terminal until the user clicked it.
    //
    // Redirection rule: walk the new focus widget's parent chain.
    //   - TerminalWidget / QDialog / QMenu / QMenuBar / CommandPalette
    //     / text-input widgets → accept focus (user legitimately meant
    //     to type into, or is interacting with, that widget).
    //   - QStatusBar / QTabBar → mark for redirect.
    //   - Everything else (bare QMainWindow, QWidget chrome) → redirect.
    // Gated on !activeModalWidget() so a modal dialog's internal focus
    // changes aren't hijacked.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (!now) return;                         // app-wide focus loss (Alt-Tab)
        if (QApplication::activeModalWidget()) return;  // modal dialog owns focus

        // Never hijack focus from a button that is still handling a
        // click. QAbstractButton emits `clicked()` only if it retains
        // focus between mousePress and mouseRelease. When this
        // lambda queued a singleShot(0) to refocus the terminal on
        // button-press, the singleShot could fire between press and
        // release, ripping focus away and silently canceling the
        // click. Symptom: user clicks "Review Changes" and nothing
        // happens — no toast, no dialog — because showDiffViewer
        // never ran. Detected 2026-04-18. Buttons own their own
        // focus lifecycle; we accept the focus, and the next
        // legitimate focusChanged (when the user clicks elsewhere)
        // will run the redirect path.
        if (qobject_cast<QAbstractButton *>(now)) return;
        // Same reasoning for "mouse is currently down" — even for
        // non-button clicks, deferring until the user releases the
        // mouse avoids racing with any widget's press/release
        // handling.
        if (QApplication::mouseButtons() != Qt::NoButton) return;

        bool shouldRedirect = true;
        for (QWidget *w = now; w; w = w->parentWidget()) {
            if (qobject_cast<TerminalWidget *>(w)) { shouldRedirect = false; break; }
            if (w->inherits("QDialog"))            { shouldRedirect = false; break; }
            if (w->inherits("QMenu") ||
                w->inherits("QMenuBar"))           { shouldRedirect = false; break; }
            if (w->inherits("CommandPalette"))     { shouldRedirect = false; break; }
            if (qobject_cast<QLineEdit *>(w) ||
                qobject_cast<QTextEdit *>(w) ||
                qobject_cast<QPlainTextEdit *>(w)) { shouldRedirect = false; break; }
            if (w->inherits("QStatusBar") ||
                w->inherits("QTabBar"))            { break; }  // keep shouldRedirect = true
        }
        if (!shouldRedirect) return;

        // Defer one tick — the focusChanged signal fires *during* Qt's
        // focus-dispatch; calling setFocus() synchronously triggers an
        // immediate recursive focusChanged that can confuse some styles.
        if (auto *t = focusedTerminal()) {
            QPointer<TerminalWidget> guard(t);
            QTimer::singleShot(0, this, [guard]() {
                if (!guard) return;
                // Re-check at firing time: if the status-bar button's
                // handler (e.g. showDiffViewer, openClaudeAllowlistDialog)
                // has since spawned a dialog, the user is now engaged
                // with that dialog and refocusing the terminal would
                // steal input focus and — on KWin with a frameless
                // parent — re-raise the main window over the freshly-
                // shown dialog.
                //
                // The focusChanged queue-time check at line ~418 couldn't
                // see this because the dialog didn't exist yet — the
                // chain was `button → QStatusBar → QMainWindow`. Walking
                // top-level widgets HERE at fire time catches dialogs
                // created between queue and fire.
                //
                // Why topLevelWidgets + isVisible instead of
                // QApplication::activeWindow(): activateWindow()'s effect
                // propagates via a platform event that, on some WMs/
                // offscreen platforms, only applies on the NEXT event-
                // loop iteration. The dialog may have been show()+raise()
                // +activateWindow()'d by the click handler yet not yet
                // be the reported activeWindow() when the singleShot
                // fires. Visibility, however, is synchronous — show()
                // sets the visible flag before returning.
                const QWidget *mainWin = guard->window();
                for (QWidget *w : QApplication::topLevelWidgets()) {
                    if (w == mainWin) continue;
                    if (!w->isVisible()) continue;
                    if (w->inherits("QDialog")) {
                        return;  // a dialog is live — don't steal its focus
                    }
                }
                guard->setFocus(Qt::OtherFocusReason);
            });
        }
    });

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

    // Dark/light mode auto-switching (Qt 6.5+ signal). Older Qt builds
    // (Ubuntu 22.04 / 24.04 LTS ship 6.2 / 6.4) lack the colorScheme()
    // accessor and colorSchemeChanged signal — feature self-disables there;
    // setting still appears in the UI but has no effect. No fallback wiring
    // (e.g. parsing GTK theme files) — too platform-specific to be worth it
    // when Qt 6.5+ is broadly available on Tumbleweed/Fedora/Arch and
    // becoming standard.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (m_config.autoColorScheme()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this]() { onSystemColorSchemeChanged(); });
        // Apply initial scheme
        onSystemColorSchemeChanged();
    }
#endif

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

#ifdef ANTS_LUA_PLUGINS
                // 0.6.9 — let plugins react to settings changes (re-read their
                // own settings, refresh status text, etc.). Payload is empty
                // because the relevant config bits are accessed via
                // ants.settings.get on demand.
                if (m_pluginManager)
                    m_pluginManager->fireEvent(PluginEvent::WindowConfigReloaded, QString());
#endif

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
    terminal->setConfirmMultilinePaste(m_config.confirmMultilinePaste());
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

#ifdef ANTS_LUA_PLUGINS
    // 0.6.9 — forward shell-integration + iTerm2 hooks out as plugin events.
    // command_finished payload: "exit_code=N&duration_ms=N" (URL-form so
    // plugins can parse with a simple split — no escaping needed).
    connect(terminal, &TerminalWidget::commandFinished, this,
            [this](int exitCode, qint64 durationMs) {
        if (m_pluginManager) {
            m_pluginManager->fireEvent(PluginEvent::CommandFinished,
                QString("exit_code=%1&duration_ms=%2").arg(exitCode).arg(durationMs));
        }
    });
    // user_var_changed payload: "NAME=value" (raw — names are already
    // identifier-shaped per the OSC 1337 SetUserVar spec).
    connect(terminal, &TerminalWidget::userVarChanged, this,
            [this](const QString &name, const QString &value) {
        if (m_pluginManager) {
            m_pluginManager->fireEvent(PluginEvent::UserVarChanged,
                                        name + QStringLiteral("=") + value);
        }
    });
    // 0.7.0 — surface OSC 133 forgery attempts in the status bar so the
    // user sees an in-terminal process trying to spoof prompt markers.
    // Throttled grid-side (5 s) so a tight forgery loop can't spam the bar.
    connect(terminal, &TerminalWidget::osc133ForgeryDetected, this,
            [this](int count) {
        showStatusMessage(
            QStringLiteral("⚠ OSC 133 forgery detected (count: %1) — an in-terminal "
                           "process tried to spoof a shell-integration marker").arg(count),
            5000);
    });
    // run_script trigger action: route the matched substring to plugins as a
    // PaletteAction event with the action id as payload. Plugins listening
    // for the matching id can dispatch their own logic.
    connect(terminal, &TerminalWidget::triggerRunScript, this,
            [this](const QString &actionId, const QString &matched) {
        if (m_pluginManager) {
            // Broadcast — any plugin can react; payload "actionId\tmatched"
            // gives the plugin both the dispatch key and the captured text.
            m_pluginManager->fireEvent(PluginEvent::PaletteAction,
                                        actionId + QStringLiteral("\t") + matched);
        }
    });
#endif

    // OSC 9;4 progress reporting — show a small colored dot as the tab icon.
    // ConEmu / Microsoft Terminal convention.
    connect(terminal, &TerminalWidget::progressChanged, this,
            [this, terminal](int state, int /*percent*/) {
        // Find which tab this terminal is in
        int tabIdx = -1;
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            QWidget *w = m_tabWidget->widget(i);
            if (w == terminal || w->isAncestorOf(terminal)) { tabIdx = i; break; }
        }
        if (tabIdx < 0) return;

        if (state == 0) {
            m_tabWidget->setTabIcon(tabIdx, QIcon());
            return;
        }
        QColor dot;
        switch (state) {
            case 1: dot = QColor(0x89, 0xB4, 0xFA); break; // Normal — blue
            case 2: dot = QColor(0xF3, 0x8B, 0xA8); break; // Error — red
            case 3: dot = QColor(0xB4, 0xBE, 0xFE); break; // Indeterminate — lavender
            case 4: dot = QColor(0xF9, 0xE2, 0xAF); break; // Warning — yellow
            default: return;
        }
        QPixmap pm(12, 12);
        pm.fill(Qt::transparent);
        QPainter pp(&pm);
        pp.setRenderHint(QPainter::Antialiasing);
        pp.setBrush(dot);
        pp.setPen(Qt::NoPen);
        pp.drawEllipse(1, 1, 10, 10);
        pp.end();
        m_tabWidget->setTabIcon(tabIdx, QIcon(pm));
    });

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

    // Claude Code permission detection → status bar notification
    connect(terminal, &TerminalWidget::claudePermissionDetected, this, [this, terminal](const QString &rawRule) {
        // Only show for the currently active tab
        if (terminal != focusedTerminal() && terminal != currentTerminal()) return;

        // Normalize and generalize the detected rule
        QString rule = ClaudeAllowlistDialog::normalizeRule(rawRule);
        QString gen = ClaudeAllowlistDialog::generalizeRule(rule);
        if (!gen.isEmpty()) rule = gen;

        // Remove any existing allowlist button to prevent accumulation.
        // Must use QWidget* not QPushButton* — the hook-path permissionRequested
        // handler creates a QWidget container (line ~2537) with objectName
        // "claudeAllowBtn"; a QPushButton-typed findChildren would miss it and
        // leave both buttons stacked when a scroll-scan detection fires while
        // a hook-path container is already visible. Mirrors the
        // onTabChanged (line ~1716) and hook-path dedup (line ~2533) lookups
        // which both already use QWidget*.
        auto existing = statusBar()->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn"));
        for (auto *btn : existing) btn->deleteLater();

        showStatusMessage(
            QString("Claude Code permission: %1 — ").arg(rule), 0);
        auto *addBtn = new QPushButton("Add to allowlist", statusBar());
        addBtn->setObjectName("claudeAllowBtn");
        // Fixed horizontal sizePolicy — the button must never be
        // squeezed when the notification slot is full of text. Same
        // layout principle as the branch chip / Claude status label
        // introduced on 2026-04-18.
        addBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        statusBar()->addPermanentWidget(addBtn);

        auto clearPromptActive = [this]() {
            m_claudePromptActive = false;
            applyClaudeStatusLabel();
        };

        connect(addBtn, &QPushButton::clicked, this, [this, rule, addBtn, clearPromptActive]() {
            openClaudeAllowlistDialog(rule);
            addBtn->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });

        // 0.6.27 — mark prompt active so the Claude status label switches
        // to "Claude: prompting". Useful when the user is scrolled up in
        // the terminal history and can't see the prompt directly.
        m_claudePromptActive = true;
        applyClaudeStatusLabel();

        // Primary retraction: terminal scrollback scanner notices the
        // footer is gone. Now debounced against transient TUI repaints
        // (see terminalwidget.cpp:checkForClaudePermissionPrompt).
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(terminal, &TerminalWidget::claudePermissionCleared, addBtn, [addBtn, conn, clearPromptActive, this]() {
            QObject::disconnect(*conn);
            addBtn->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });

        // 0.6.33 — belt-and-suspenders retraction parity with the hook
        // path (see line ~2676). If the terminal scanner never notices
        // the prompt clearing (unmatched footer format on a future
        // Claude Code release; prompt scrolled off the 12-line lookback
        // before the debounce settled), toolFinished / sessionStopped
        // give us a resolve signal so the button doesn't linger. Same
        // reasoning as the hook path: errs on the side of closing the
        // button too early rather than leaving a stale "Add to
        // allowlist" stranded on the bar after the user already
        // approved.
        if (m_claudeIntegration) {
            auto finishedConn = std::make_shared<QMetaObject::Connection>();
            *finishedConn = connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                                    addBtn, [addBtn, finishedConn, clearPromptActive, this](const QString &, bool) {
                QObject::disconnect(*finishedConn);
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            });
            auto stoppedConn = std::make_shared<QMetaObject::Connection>();
            *stoppedConn = connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                                   addBtn, [addBtn, stoppedConn, clearPromptActive, this](const QString &) {
                QObject::disconnect(*stoppedConn);
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            });
        }
    });
}

void MainWindow::newTab() {
    // Inherit CWD from the currently focused terminal
    QString inheritCwd;
    if (auto *prev = focusedTerminal())
        inheritCwd = prev->shellCwd();
    else if (auto *fallback = currentTerminal())
        inheritCwd = fallback->shellCwd();

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

    // Drop any persisted tab-colour entry before we forget the UUID —
    // otherwise the config would accumulate orphan entries for closed
    // tabs that no future tab will ever re-use (UUIDs are unique).
    {
        QString tabId = m_tabSessionIds.value(w);
        if (!tabId.isEmpty()) {
            QJsonObject groups = m_config.tabGroups();
            if (groups.contains(tabId)) {
                groups.remove(tabId);
                m_config.setTabGroups(groups);
            }
        }
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

void MainWindow::onTabChanged(int index) {
    // All per-tab status-bar state — branch chip, process name,
    // notification slot, Claude state, Review Changes button, Add-to-
    // allowlist button — funnels through a single refresh point so
    // nothing bleeds from the previous tab. See
    // refreshStatusBarForActiveTab() for the lifecycle contract.
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (t) {
        t->setFocus();
        onTitleChanged(t->shellTitle());
    }
    refreshStatusBarForActiveTab();

#ifdef ANTS_LUA_PLUGINS
    // 0.6.9 — fire `pane_focused` so plugins can swap context (per-pane
    // status, badge, ssh-connection-aware behavior). Today this fires on
    // tab switches; once split-pane focus tracking lands the same event
    // covers within-tab pane changes without further plugin churn.
    if (m_pluginManager) {
        QString tabTitle = (index >= 0 && index < m_tabWidget->count())
                           ? m_tabWidget->tabText(index) : QString();
        m_pluginManager->fireEvent(PluginEvent::PaneFocused, tabTitle);
    }
#endif
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
    //
    // Qt stylesheet cascade: a stylesheet set on QMainWindow applies to its
    // QObject descendants, which includes child QDialogs — so the dialog
    // selectors below reach every popup (Settings, Audit, AI, SSH, Claude*,
    // QMessageBox/QInputDialog/etc.) as long as they were created with the
    // main window as their parent. Untagged QDialog must therefore stay
    // anchor-selected here and not rely on dialog-local setStyleSheet().
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
        // 0.6.27 — was `image: none`, which hid the × glyph entirely and
        // left only an invisible hover hit-target. Users didn't discover
        // the close affordance unless they happened to mouse over it.
        // Removing the `image` rule lets Qt fall back to the platform
        // close icon (QStyle::SP_TitleBarCloseButton / SP_DockWidgetCloseButton)
        // which adapts to the active QPalette. The 14×14 box + margins
        // give the × breathing room; hover still applies an ansi-red
        // background (%7) for "will-click" feedback.
        "QTabBar::close-button {"
        "  subcontrol-position: right; margin: 2px; padding: 1px;"
        "  width: 14px; height: 14px; border-radius: 3px; }"
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

        // ---- Pop-up / dialog theming ----
        // QMessageBox, QInputDialog, QColorDialog, QFileDialog, and our own
        // QDialog subclasses (Settings/Audit/AI/SSH/Claude*) all cascade from
        // here. Colors match the terminal's active theme.
        "QDialog { background-color: %2; color: %3; }"
        "QLabel { color: %3; background: transparent; }"
        "QPushButton { background-color: %1; color: %3;"
        "  border: 1px solid %4; padding: 6px 14px; border-radius: 4px; min-width: 60px; }"
        // :hover must be gated by :enabled. Without the gate, a disabled
        // button still gets the hover highlight, which advertises it as
        // actionable even though Qt swallows clicks on disabled buttons
        // (QAbstractButton::mousePressEvent early-returns). The Review
        // Changes button on a clean repo is the canonical example: it's
        // visible-but-disabled to tell the user "the repo is clean,"
        // but without this gate the hover highlight lied and made the
        // user think the button should work. See
        // tests/features/review_changes_clickable/spec.md.
        "QPushButton:hover:enabled { background-color: %5; color: %2; border-color: %5; }"
        "QPushButton:pressed:enabled { background-color: %4; }"
        "QPushButton:default { border: 1px solid %5; }"
        "QPushButton:disabled { color: %6; border-color: %4; background-color: %2; }"
        "QLineEdit { background-color: %1; color: %3; border: 1px solid %4;"
        "  padding: 5px 8px; border-radius: 3px;"
        "  selection-background-color: %5; selection-color: %2; }"
        "QLineEdit:focus { border: 1px solid %5; }"
        "QLineEdit:disabled { color: %6; }"
        "QTextEdit, QPlainTextEdit, QTextBrowser { background-color: %1; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2; }"
        "QCheckBox { color: %3; spacing: 6px; background: transparent; }"
        "QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %4;"
        "  background: %1; border-radius: 3px; }"
        "QCheckBox::indicator:checked { background: %5; border-color: %5; }"
        "QRadioButton { color: %3; spacing: 6px; background: transparent; }"
        "QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %4;"
        "  background: %1; border-radius: 7px; }"
        "QRadioButton::indicator:checked { background: %5; border-color: %5; }"
        "QComboBox { background-color: %1; color: %3; border: 1px solid %4;"
        "  padding: 4px 8px; border-radius: 3px; min-width: 80px; }"
        "QComboBox:hover { border-color: %5; }"
        "QComboBox::drop-down { border: none; width: 20px; }"
        "QComboBox QAbstractItemView { background-color: %2; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2;"
        "  outline: none; }"
        "QSpinBox, QDoubleSpinBox { background-color: %1; color: %3;"
        "  border: 1px solid %4; padding: 4px 6px; border-radius: 3px; }"
        "QSpinBox:focus, QDoubleSpinBox:focus { border-color: %5; }"
        "QGroupBox { color: %3; border: 1px solid %4; border-radius: 4px;"
        "  margin-top: 10px; padding-top: 8px; background: transparent; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px;"
        "  padding: 0 4px; color: %3; }"
        "QListWidget, QTreeWidget, QTableWidget { background-color: %1; color: %3;"
        "  border: 1px solid %4; selection-background-color: %5; selection-color: %2;"
        "  alternate-background-color: %2; outline: none; }"
        "QListWidget::item:hover, QTreeWidget::item:hover, QTableWidget::item:hover"
        "  { background: %2; }"
        "QHeaderView::section { background-color: %2; color: %3;"
        "  border: 1px solid %4; padding: 4px 8px; }"
        "QScrollBar:vertical { background-color: %2; width: 12px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background-color: %4; border-radius: 6px;"
        "  min-height: 20px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background-color: %5; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QScrollBar:horizontal { background-color: %2; height: 12px; margin: 0; border: none; }"
        "QScrollBar::handle:horizontal { background-color: %4; border-radius: 6px;"
        "  min-width: 20px; margin: 2px; }"
        "QScrollBar::handle:horizontal:hover { background-color: %5; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }"
        "QToolTip { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        "QProgressBar { background-color: %1; color: %3; border: 1px solid %4;"
        "  border-radius: 3px; text-align: center; }"
        "QProgressBar::chunk { background-color: %5; }"
        "QDialogButtonBox QPushButton { min-width: 80px; }"
    ).arg(theme.bgPrimary.name(),
          theme.bgSecondary.name(),
          theme.textPrimary.name(),
          theme.border.name(),
          theme.accent.name(),
          theme.textSecondary.name(),
          theme.ansi[1].name());  // ANSI red for close/danger

    setStyleSheet(ss);

    // Cascade to any already-open top-level dialog that was created with
    // this MainWindow as parent. Qt already propagates via the object tree,
    // but dialogs cached across theme changes (m_settingsDialog, m_aiDialog,
    // m_auditDialog …) are re-polished here so the live widgets pick up the
    // new palette without needing to re-instantiate.
    for (QWidget *child : findChildren<QDialog *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
        child->update();
    }

    m_titleBar->setThemeColors(theme.bgSecondary, theme.textPrimary,
                                theme.accent, theme.border, theme.ansi[1]);

    // Status bar labels use theme colors (null-guarded for first call during construction)
    QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");
    // Git branch chip: rounded background + border + distinct color.
    // Uses theme.ansi[6] (cyan) so the chip never collides with the Claude
    // status label's green (idle), yellow (prompting/tool), blue (thinking),
    // or magenta (compacting) vocabulary, and never collides with the
    // transient notification's textPrimary foreground. User feedback
    // 2026-04-18: "please change the colour of the git branch so as to
    // not match any status bar notification."
    if (m_statusGitBranch)
        m_statusGitBranch->setStyleSheet(
            QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid %3; "
                          "border-radius: 3px; padding: 1px 8px; margin: 2px 6px 2px 4px; "
                          "font-size: 11px; font-weight: 600; }")
                .arg(theme.ansi[6].name(), theme.bgSecondary.name(), theme.border.name()));
    if (m_statusGitSep)
        // Hard divider between the branch chip and the transient-status slot.
        // textSecondary (not border) because border is often nearly invisible
        // against bgPrimary on low-contrast themes (Gruvbox-dark, Nord, Solarized);
        // textSecondary is the muted-but-readable role that every theme tunes
        // for foreground legibility. Using background-color (not palette) to
        // paint the QFrame::VLine works around Fusion's habit of drawing
        // VLines in the window's palette midlight/dark roles, which also
        // disappear on dark themes.
        m_statusGitSep->setStyleSheet(
            QStringLiteral("QFrame { background-color: %1; border: none; }")
                .arg(theme.textSecondary.name()));
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

#ifdef ANTS_LUA_PLUGINS
    // 0.6.9 — fire `theme_changed` event so plugins can swap palette/icon
    // assets, redraw status-bar widgets, etc. Payload is the new theme name.
    if (m_pluginManager) m_pluginManager->fireEvent(PluginEvent::ThemeChanged, name);
#endif

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
        // Re-apply any persisted colour tag for this UUID. Must happen
        // after addTab (tab has an index) and after m_tabSessionIds is
        // populated (so applyPersistedTabColor can resolve the UUID).
        applyPersistedTabColor(terminal);

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

        // Drain the event queue so QTabWidget's layout (including
        // QStackedWidget's propagation to all pages) and the main
        // window's show-event sequence have completed before we
        // trigger per-tab shell startup. Without this, inactive tab
        // pages may still carry their default-constructed tiny
        // geometry, and startShell → recalcGridSize would reflow
        // their grids to ~3x10, pushing blank rows into scrollback.
        //
        // A second processEvents call catches any layout events
        // that the first iteration queued (layout can take multiple
        // passes when the main window also re-polishes its
        // stylesheet). TerminalWidget::recalcGridSize additionally
        // has a pre-layout guard (see src/terminalwidget.cpp
        // recalcGridSize) so genuinely-unlaid-out widgets don't
        // reflow, but draining here is cheap and catches the
        // common path too.
        QApplication::processEvents();
        QApplication::processEvents();

        for (const auto &tab : restoredTabs) {
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

// 0.6.27 — central renderer for the "Claude: …" status-bar label. Kept
// as a single method so the permission-prompt overlay ("Claude:
// prompting") and the underlying ClaudeIntegration state ("Claude: idle
// / thinking / tool-use / compacting") share one code path and stay in
// sync. User rationale (2026-04-15): when scrolled up in scrollback
// reading history, the user can't see Claude's live prompt — the
// status-bar label is the only at-a-glance signal that a response is
// waiting on them, so "idle" misrepresents the state.
void MainWindow::applyClaudeStatusLabel() {
    if (!m_claudeStatusLabel) return;
    const Theme &th = Themes::byName(m_currentTheme);
    const QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");

    // NotRunning hides both label and the context progress bar regardless
    // of prompt state — no Claude process = nothing to announce.
    if (m_claudeLastState == ClaudeState::NotRunning) {
        m_claudeStatusLabel->hide();
        if (m_claudeContextBar) m_claudeContextBar->hide();
        return;
    }

    QString text;
    QColor color;

    // Curated status vocabulary (user spec 2026-04-18):
    //   idle / thinking / prompting / bash / reading a file / planning /
    //   auditing / compacting.
    // Color legend, each distinct from both the branch chip (cyan) and
    // the transient notification (textPrimary):
    //   idle      → green   (ansi[2])   "ready, waiting on you"
    //   thinking  → blue    (ansi[4])   "internal reasoning"
    //   prompting → yellow  (ansi[3])   "needs your decision"
    //   tool use  → yellow  (ansi[3])   also "active work"
    //   planning  → ansi[6] (cyan-ish)  "read-only scoping"
    //   auditing  → ansi[5] (magenta)   "sweeping for issues"
    //   compacting→ ansi[5] (magenta)   "rewriting history"
    if (m_claudePromptActive) {
        // Prompt-active overrides the base state — a waiting permission
        // prompt is what the user needs to see.
        text = QStringLiteral("Claude: prompting");
        color = th.ansi[3];
    } else if (m_claudePlanMode) {
        // Plan mode is a user-selected interaction mode (Shift+Tab),
        // orthogonal to the transcript-derived state. While in plan mode
        // the assistant can think/read but cannot edit or run commands —
        // "planning" is the honest label.
        text = QStringLiteral("Claude: planning");
        color = th.ansi[6];
    } else if (m_claudeAuditing) {
        // Auditing is detected from a recent user message that invoked
        // the /audit skill in the transcript. Lives beside state because
        // the user can audit during tool use, thinking, or idle — the
        // skill's lifecycle is not the same as any single tool.
        text = QStringLiteral("Claude: auditing");
        color = th.ansi[5];
    } else {
        switch (m_claudeLastState) {
        case ClaudeState::NotRunning:
            return;  // handled above
        case ClaudeState::Idle:
            text = QStringLiteral("Claude: idle");
            color = th.ansi[2];
            break;
        case ClaudeState::Thinking:
            text = QStringLiteral("Claude: thinking");
            color = th.ansi[4];
            break;
        case ClaudeState::ToolUse: {
            // Map tool name → friendly vocabulary per user spec. Unknown
            // tools fall through to the raw name so MCP / custom tools
            // remain legible. Comparison is case-insensitive because
            // transcript tool names and hook tool names have historically
            // differed in casing across Claude Code releases.
            const QString t = m_claudeLastDetail.trimmed();
            const QString lower = t.toLower();
            if (lower == QLatin1String("bash")) {
                text = QStringLiteral("Claude: bash");
            } else if (lower == QLatin1String("read")) {
                text = QStringLiteral("Claude: reading a file");
            } else if (lower == QLatin1String("edit") ||
                       lower == QLatin1String("write") ||
                       lower == QLatin1String("notebookedit")) {
                text = QStringLiteral("Claude: editing");
            } else if (lower == QLatin1String("grep") ||
                       lower == QLatin1String("glob")) {
                text = QStringLiteral("Claude: searching");
            } else if (lower == QLatin1String("webfetch") ||
                       lower == QLatin1String("websearch")) {
                text = QStringLiteral("Claude: browsing");
            } else if (lower == QLatin1String("task") ||
                       lower == QLatin1String("agent")) {
                text = QStringLiteral("Claude: delegating");
            } else if (t.isEmpty()) {
                text = QStringLiteral("Claude: thinking");
            } else {
                text = QStringLiteral("Claude: %1").arg(t);
            }
            color = th.ansi[3];
            break;
        }
        case ClaudeState::Compacting:
            text = QStringLiteral("Claude: compacting");
            color = th.ansi[5];
            break;
        }
    }

    m_claudeStatusLabel->setText(text);
    m_claudeStatusLabel->setStyleSheet(
        QStringLiteral("color: %1; %2").arg(color.name(), statusStyle));
    m_claudeStatusLabel->show();
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
    if (m_claudeReviewBtn) {
        // 0.6.26 — side-by-side with the "Add to allowlist" button in the
        // status bar, the custom-styled Review Changes button looked wildly
        // out of place (see screenshot attached to the original report).
        // "Add to allowlist" is created at mainwindow.cpp:1234 with *no*
        // stylesheet, so it inherits the global QPushButton rule from
        // mainwindow.cpp:1625-1630 — that's the target styling. Stop
        // over-styling the enabled state: reset the font to Qt's default,
        // clear any fixedHeight so the size-hint matches its sibling.
        //
        // Disabled state — the button stays visible on clean git repos so
        // the user still sees "Claude edited something" (see
        // refreshReviewButton at mainwindow.cpp:~2763). The visual must
        // clearly read as non-actionable without shouting. Three layered
        // cues: italic text (typographic "this is passive"), dashed border
        // (borrowed from common desktop-toolkit conventions for disabled
        // chip buttons), and textSecondary on bgSecondary (muted palette).
        // Only the :disabled selector is set on the widget, so the global
        // QPushButton enabled/hover/pressed rules still apply for the
        // enabled state — no duplication, no drift.
        //
        // Palette force-set: survives the "dim on Gruvbox" contrast issue
        // on the enabled state regardless of how Qt composites the text
        // rect on a statusbar-parented widget (pre-0.6.26 this was
        // rendered dim even with the stylesheet's color property set —
        // root cause: platform style composited a reduced-alpha overlay).
        m_claudeReviewBtn->setFont(QFont());
        m_claudeReviewBtn->setMinimumHeight(0);
        m_claudeReviewBtn->setMaximumHeight(QWIDGETSIZE_MAX);
        m_claudeReviewBtn->setStyleSheet(QStringLiteral(
            "QPushButton:disabled {"
            "  color: %1;"
            "  background-color: %2;"
            "  border: 1px dashed %3;"
            "  font-style: italic;"
            "}").arg(th.textSecondary.name(),
                     th.bgSecondary.name(),
                     th.border.name()));

        QPalette pal = m_claudeReviewBtn->palette();
        pal.setColor(QPalette::Active,   QPalette::ButtonText, th.textPrimary);
        pal.setColor(QPalette::Inactive, QPalette::ButtonText, th.textPrimary);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, th.textSecondary);
        pal.setColor(QPalette::Active,   QPalette::WindowText, th.textPrimary);
        pal.setColor(QPalette::Inactive, QPalette::WindowText, th.textPrimary);
        pal.setColor(QPalette::Disabled, QPalette::WindowText, th.textSecondary);
        m_claudeReviewBtn->setPalette(pal);
    }
    if (m_claudeErrorLabel)
        m_claudeErrorLabel->setStyleSheet(QStringLiteral("color: %1; padding: 0 4px; font-size: 11px;").arg(th.ansi[1].name()));
}

void MainWindow::setupClaudeIntegration() {
    m_claudeIntegration = new ClaudeIntegration(this);

    // Status bar indicator for Claude Code state. Plain QLabel with Fixed
    // horizontal sizePolicy — the vocabulary is a bounded set of short
    // labels ("Claude: idle", "Claude: thinking...", "Claude: bash",
    // "Claude: reading a file", "Claude: planning", "Claude: auditing",
    // "Claude: prompting", "Claude: compacting..."), so the widget can
    // grow to fit its natural width without ever needing to elide. The
    // widget is hidden when the tab's shell has no Claude process.
    m_claudeStatusLabel = new QLabel(this);
    m_claudeStatusLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
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

    // Review Changes button (shown when Claude edits files). Size/height
    // intentionally left at Qt's default so it matches the sibling
    // "Add to allowlist" button (mainwindow.cpp:1234) that inherits the
    // global QPushButton stylesheet. updateClaudeThemeColors() applies the
    // palette force-set for contrast without adding compact-height
    // overrides that would re-introduce the size mismatch.
    m_claudeReviewBtn = new QPushButton("Review Changes", this);
    // Fixed horizontal sizePolicy — never squeezed for the benefit of
    // the notification slot. See layout contract at mainwindow.cpp:~320.
    m_claudeReviewBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
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
        m_claudeLastState = state;
        m_claudeLastDetail = detail;
        applyClaudeStatusLabel();
    });

    connect(m_claudeIntegration, &ClaudeIntegration::planModeChanged,
            this, [this](bool active) {
        m_claudePlanMode = active;
        applyClaudeStatusLabel();
    });

    connect(m_claudeIntegration, &ClaudeIntegration::auditingChanged,
            this, [this](bool active) {
        m_claudeAuditing = active;
        applyClaudeStatusLabel();
    });

    connect(m_claudeIntegration, &ClaudeIntegration::contextUpdated,
            this, [this](int percent) {
        // 0.6.26 — contextUpdated(0) is emitted by ClaudeIntegration::setShellPid
        // on every tab switch as part of the state reset (alongside
        // stateChanged(NotRunning)). Previously we called show() here
        // unconditionally, which re-exposed the bar at 0% immediately after
        // the NotRunning handler had hidden it — so a fresh tab (or a tab
        // where Claude was never started) still painted a "0%" widget in
        // the status bar. Treat 0 as "no session / nothing to show" and
        // hide. The bar only re-appears once Claude emits a real,
        // non-zero context percentage (claudeintegration.cpp:333-334).
        if (percent <= 0) {
            m_claudeContextBar->hide();
            return;
        }
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
        // 0.6.22 — refreshReviewButton decides visibility + enabled state:
        //   * not a git repo (or no cwd) → hidden entirely
        //   * git repo with no diff     → visible but disabled
        //   * git repo with diff        → visible and enabled
        // This replaces the old unconditional show() which could leave
        // the button visible in non-git contexts (where clicking it
        // only produced a "No changes detected" toast).
        refreshReviewButton();
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

        // Dedup: a PermissionRequest hook that arrives while a scroll-scan
        // permission button is already on screen should not stack a second
        // button group beside it. Remove any existing "claudeAllowBtn"
        // widgets (from either path) first — same objectName as the
        // scroll-scan path so the onTabChanged cleanup catches both.
        for (auto *w : statusBar()->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn")))
            w->deleteLater();

        // Enhanced permission action buttons
        auto *btnWidget = new QWidget(statusBar());
        // 0.6.29 — same objectName as the scroll-scan path's button
        // (see line ~1342) so the tab-switch cleanup in onTabChanged
        // (line ~1643) removes both. Previously this widget had no
        // objectName, so switching tabs mid-prompt left a stranded
        // button group visible on the wrong tab.
        btnWidget->setObjectName(QStringLiteral("claudeAllowBtn"));
        // Fixed horizontal sizePolicy — must never be squeezed when
        // the notification slot is wide. See layout principle at
        // mainwindow.cpp:~320 (user spec 2026-04-18).
        btnWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
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

        // Mark prompt active so the Claude status label switches to
        // "prompting" — matches the scroll-scan path's behavior and
        // gives the user a second at-a-glance indicator beyond the
        // button group itself.
        m_claudePromptActive = true;
        applyClaudeStatusLabel();

        auto clearPromptActive = [this]() {
            m_claudePromptActive = false;
            applyClaudeStatusLabel();
        };

        connect(allowBtn, &QPushButton::clicked, btnWidget, [this, btnWidget, clearPromptActive]() {
            btnWidget->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });
        connect(denyBtn, &QPushButton::clicked, btnWidget, [this, btnWidget, clearPromptActive]() {
            btnWidget->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });
        connect(addBtn, &QPushButton::clicked, this, [this, rule, btnWidget, clearPromptActive]() {
            openClaudeAllowlistDialog(rule);
            btnWidget->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        });

        // Remove buttons when the prompt disappears from the screen.
        // Same lesson as the grid-scan path above: don't tie retraction to
        // `outputReceived`, which fires on every repaint and would retract
        // the buttons while the prompt is still visible. `claudePermissionCleared`
        // fires only on the transition to "no prompt on screen".
        //
        // Listen on ALL terminals (not just currentTerminal) so a prompt
        // raised via hook on tab A disappears when the user approves/declines
        // in tab A even after briefly visiting tab B. Multiple connects are
        // fine; each disconnects itself via the shared pointer once fired.
        for (auto *term : m_tabWidget->findChildren<TerminalWidget *>()) {
            auto conn = std::make_shared<QMetaObject::Connection>();
            *conn = connect(term, &TerminalWidget::claudePermissionCleared,
                            btnWidget, [btnWidget, conn, clearPromptActive]() {
                QObject::disconnect(*conn);
                btnWidget->deleteLater();
                clearPromptActive();
            });
        }

        // 0.6.31 — `claudePermissionCleared` above only fires if the terminal
        // scroll-scanner previously emitted `claudePermissionDetected` for
        // this prompt (gated on `m_lastDetectedRule` being non-empty in
        // terminalwidget.cpp:3658). When a PermissionRequest HOOK fires for
        // a prompt the scroll-scanner never saw — unmatched prompt format,
        // prompt already scrolled past the 12-line lookback window, or a
        // headless Claude Code session where the hook is the only signal
        // — `m_lastDetectedRule` stays empty and `claudePermissionCleared`
        // never fires, orphaning the button forever. User-reported symptom:
        // "Claude Code permission: Bash(cd * && cmake * | tail *) —" visible
        // with no live prompt in any terminal.
        //
        // Retract on `toolFinished` (permission was granted and the tool
        // completed), `sessionStopped` (session ended — prompt is moot),
        // and `permissionRequested` (a new prompt implicitly resolves the
        // previous one; the existing `findChildren` dedup at the top of
        // this handler already removes the old btnWidget, which auto-
        // disconnects these connections via the btnWidget context).
        //
        // These are proxy signals — Claude Code has no canonical
        // "PermissionResolved" hook (confirmed in claudeintegration.cpp
        // processHookEvent; PermissionRequest has no inverse). Using
        // toolFinished/sessionStopped errs on the side of closing the
        // button too early (user never clicks it) rather than too late
        // (button lingers indefinitely on a resolved prompt, inviting a
        // misdirected click).
        auto finishedConn = std::make_shared<QMetaObject::Connection>();
        *finishedConn = connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                                btnWidget, [btnWidget, finishedConn, clearPromptActive](const QString &, bool) {
            QObject::disconnect(*finishedConn);
            btnWidget->deleteLater();
            clearPromptActive();
        });
        auto stoppedConn = std::make_shared<QMetaObject::Connection>();
        *stoppedConn = connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                               btnWidget, [btnWidget, stoppedConn, clearPromptActive](const QString &) {
            QObject::disconnect(*stoppedConn);
            btnWidget->deleteLater();
            clearPromptActive();
        });
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
        // Auto-save immediately so Claude Code picks up the rule right away.
        // Surface a specific error if the save failed (permissions, disk full,
        // settings.local.json on a read-only mount). Previously the return
        // value was ignored and the "rule added" toast always appeared even
        // when the write silently failed — user reported "Add to allowlist
        // does nothing" with the save failing against a read-only .claude
        // directory inherited from a worktree checkout.
        if (m_claudeDialog->saveSettings()) {
            showStatusMessage(
                QString("Rule added to allowlist → %1").arg(settingsPath), 5000);
        } else {
            showStatusMessage(
                QString("Could not write allowlist: %1 (check permissions)").arg(settingsPath),
                8000);
        }
    }
    m_claudeDialog->show();
    // raise() + activateWindow() are load-bearing, not cosmetic — same
    // constraint as showDiffViewer (see the comment at the end of that
    // function). The "Add to allowlist" button that invokes this dialog
    // lives on the status bar of a frameless QMainWindow. KWin's window
    // stacking on a frameless parent, combined with the focusChanged
    // redirect lambda at line ~411 that queues a terminal->setFocus()
    // when the status-bar button briefly takes focus, places the dialog
    // BEHIND the main window unless we both raise() and activateWindow().
    // raise() fixes stacking order; activateWindow() makes the dialog the
    // input-focus target so the queued terminal-refocus becomes a no-op
    // (the dialog-visible check at line ~464 sees it and bails). Without
    // activateWindow(), the user reports: "Add to allowlist click does
    // nothing — no dialog opens, no visible effect" — the dialog IS up,
    // just obscured by the main window.
    m_claudeDialog->raise();
    m_claudeDialog->activateWindow();
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
    // Persist tab color sequence unconditionally — independent of
    // session persistence, so the fallback restore path can apply
    // colors at next launch even when scrollback isn't saved.
    saveTabColorSequence();
    saveAllSessions();
    event->accept();
}

// --- Status bar ---

void MainWindow::showStatusMessage(const QString &msg, int timeoutMs) {
    // Label is created in the constructor before anything that can emit a status
    // message, so a null check here would just mask bugs.
    if (!m_statusMessage) return;
    m_statusMessage->setFullText(msg);
    if (m_statusMessageTimer) m_statusMessageTimer->stop();
    // Negative sentinel = use configured default (user spec 2026-04-18:
    // "Should have a timeout that can be adjusted in the settings but
    // with a default of 5 seconds").
    if (timeoutMs < 0) timeoutMs = m_config.notificationTimeoutMs();
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
    if (m_statusMessage) m_statusMessage->setFullText(QString());
    if (m_statusMessageTimer) m_statusMessageTimer->stop();
}

void MainWindow::refreshStatusBarForActiveTab() {
    // Single per-tab refresh point. Every status-bar widget falls into
    // exactly one of three lifecycle categories:
    //
    //   A. State widgets (branch chip, process, Claude status, Review
    //      Changes button). Always re-computed from the new active
    //      tab's terminal. If the info is absent for this tab, the
    //      widget hides — it never carries data from the previous tab.
    //
    //   B. Transient notifications (m_statusMessage). The transient
    //      message belongs to the tab it was fired on; switching tabs
    //      cancels it. (Users explicitly requested this on 2026-04-18.)
    //
    //   C. Event-tied widgets (Add-to-allowlist button, transient
    //      Claude error label). Visible only while the originating
    //      event is live on its tab. Tab switch destroys them — the
    //      next permission prompt will re-create a fresh instance
    //      against whichever tab is then active.
    //
    // Called from: onTabChanged, plus any place that wants to force a
    // full refresh (fileChanged, post-approve/-decline on allowlist,
    // etc.). Cheap: just reads cached values and schedules the async
    // git probe.
    const bool haveStatus = (m_statusGitBranch && m_statusProcess);

    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();

    // Category B: always cancel transient notifications on tab switch.
    clearStatusMessage();

    // Category C: event-tied widgets die on tab switch.
    if (m_claudeErrorLabel) m_claudeErrorLabel->hide();
    // QWidget (not QPushButton) — the hook-server path uses a QWidget
    // container named "claudeAllowBtn" holding Allow/Deny/Add-to-allowlist
    // children; the scroll-scan path creates a bare QPushButton with the
    // same objectName. Finding by QWidget covers both.
    const auto staleAllowBtns =
        statusBar()->findChildren<QWidget *>(QStringLiteral("claudeAllowBtn"));
    for (QWidget *w : staleAllowBtns) w->deleteLater();
    if (m_claudePromptActive) {
        m_claudePromptActive = false;  // applyClaudeStatusLabel called below
    }

    // No active terminal (last tab closed, mid-teardown) — clear every
    // Category A widget so the bar doesn't show stale data.
    if (!t) {
        if (haveStatus) {
            m_statusGitBranch->clear();
            m_statusGitBranch->hide();
            if (m_statusGitSep) m_statusGitSep->hide();
            m_statusProcess->clear();
            m_statusProcess->hide();
        }
        if (m_claudeIntegration)
            m_claudeIntegration->setShellPid(0);
        m_claudeLastState = ClaudeState::NotRunning;
        m_claudeLastDetail.clear();
        m_claudePlanMode = false;
        m_claudeAuditing = false;
        applyClaudeStatusLabel();
        if (m_claudeReviewBtn) m_claudeReviewBtn->hide();
        if (m_claudeContextBar) m_claudeContextBar->hide();
        return;
    }

    // Category A: re-probe state widgets against the new tab.
    //   - updateStatusBar() handles branch chip + process name
    //     synchronously (both are cheap file reads).
    //   - setShellPid() kicks Claude Integration to re-detect Claude
    //     under this tab's shell; it emits stateChanged signals that
    //     flow back into applyClaudeStatusLabel via the existing
    //     connection. State is CLEARED inside setShellPid when the PID
    //     changes, so the label never carries over from the previous
    //     tab. See claudeintegration.cpp:58-71.
    //   - refreshReviewButton() spawns an async `git status` probe;
    //     the button is hidden immediately and revealed only when the
    //     probe confirms the new tab's cwd is a git repo.
    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(t->shellPid());
    // Plan / auditing flags are derived from the transcript and will
    // be refreshed by the next ClaudeIntegration stateChanged signal,
    // but clear them now so the wrong-tab's flags don't briefly show
    // until that signal arrives.
    m_claudePlanMode = false;
    m_claudeAuditing = false;
    applyClaudeStatusLabel();
    updateStatusBar();
    refreshReviewButton();
}

void MainWindow::updateStatusBar() {
    if (!m_statusGitBranch || !m_statusProcess)
        return;

    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (!t) {
        // No active terminal — clear every per-tab widget so nothing
        // bleeds from a previously-active tab after the last tab closes.
        m_statusGitBranch->clear();
        m_statusGitBranch->hide();
        if (m_statusGitSep) m_statusGitSep->hide();
        m_statusProcess->clear();
        m_statusProcess->hide();
        return;
    }

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
        if (m_statusGitSep) m_statusGitSep->show();
    } else {
        m_statusGitBranch->clear();
        m_statusGitBranch->hide();
        if (m_statusGitSep) m_statusGitSep->hide();
    }

    // Foreground process
    QString proc = t->foregroundProcess();
    if (!proc.isEmpty()) {
        m_statusProcess->setText(proc);
        m_statusProcess->show();
    } else {
        m_statusProcess->clear();
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

    // m_quakeAnim is reused across hide/show toggles. Previously we
    // connected finished→hide() in the hide branch with UniqueConnection;
    // Qt can't dedupe lambdas, and even if it could, the same slot is
    // needed on both branches' end-states (animation end = whatever the
    // current "done" action is). On the show branch the stale
    // finished→hide() connection from a prior hide fired right after the
    // slide-down animation completed — the window would appear for 200 ms
    // and then vanish. Fix: disconnect all finished() slots before every
    // start(), and only add the hide() slot on the hide branch.
    if (!m_quakeAnim) {
        m_quakeAnim = new QPropertyAnimation(this, "pos", this);
        m_quakeAnim->setDuration(200);
    }
    m_quakeAnim->stop();
    QObject::disconnect(m_quakeAnim, &QPropertyAnimation::finished, this, nullptr);

    if (m_quakeVisible) {
        // Slide up (hide)
        m_quakeAnim->setEasingCurve(QEasingCurve::InQuad);
        m_quakeAnim->setStartValue(pos());
        m_quakeAnim->setEndValue(QPoint(geo.x(), geo.y() - h));
        connect(m_quakeAnim, &QPropertyAnimation::finished, this, [this]() {
            hide();
        });
        m_quakeAnim->start();
        m_quakeVisible = false;
    } else {
        // Slide down (show) — no finished() slot needed.
        move(geo.x(), geo.y() - h);
        show();
        raise();
        activateWindow();
        m_quakeAnim->setEasingCurve(QEasingCurve::OutQuad);
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
    } else if (actionType == "sound" || actionType == "bell") {
        QApplication::beep();
    } else if (actionType == "command") {
        if (!actionValue.isEmpty()) {
            QProcess::startDetached("/bin/sh", {"-c", actionValue});
        }
    } else if (actionType == "inject") {
        // Inject text directly into the focused PTY. \n / \r in the action
        // value pass through verbatim so a "yes\n" rule can auto-answer a
        // prompt — caller's responsibility to scope this with a tight regex.
        if (auto *t = focusedTerminal()) {
            t->sendToPty(actionValue.toUtf8());
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
            if (idx < 0 || !m_coloredTabBar) return;
            // ColoredTabBar stores the colour in QTabBar::tabData, which
            // survives drag-reorder and auto-drops when a tab is
            // removed. No MainWindow-side bookkeeping required for the
            // in-session state.
            m_coloredTabBar->setTabColor(idx, ce.color);
            // Persist the choice to config so it survives a restart.
            // Keyed by the tab's UUID (m_tabSessionIds), NOT its index —
            // indices go stale on drag-reorder but UUIDs are stable for
            // the lifetime of the tab.
            persistTabColor(tabWidget, ce.color);
        });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::persistTabColor(QWidget *tabRoot, const QColor &color) {
    // Resolve this tab's UUID. For split-pane tabs the root widget is a
    // QSplitter which holds the UUID; for single-pane tabs it's the
    // TerminalWidget itself. Both paths funnel through m_tabSessionIds.
    const QString tabId = m_tabSessionIds.value(tabRoot);
    if (tabId.isEmpty()) return;

    QJsonObject groups = m_config.tabGroups();
    if (color.isValid()) {
        // Store as "#rrggbbaa" so alpha round-trips losslessly. The
        // colour-picker entries are all alpha=255, but storing the alpha
        // keeps the format future-proof if a custom-colour entry lands
        // later.
        groups[tabId] = color.name(QColor::HexArgb);
    } else {
        // None / clear — drop the entry entirely so the JSON doesn't
        // accumulate empty strings for every tab the user ever touched.
        groups.remove(tabId);
    }
    m_config.setTabGroups(groups);

    // Mirror the change into the ordered fallback list so colors
    // survive restart even with session persistence disabled. The UUID
    // map above still wins when UUIDs match (session persistence on,
    // drag-reorder within a session); the ordered list is only
    // consulted as a fallback at startup.
    saveTabColorSequence();
}

void MainWindow::saveTabColorSequence() {
    if (!m_coloredTabBar) return;
    QJsonArray seq;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        const QColor c = m_coloredTabBar->tabColor(i);
        // Empty string = uncolored slot; preserve the index so later
        // tabs' colors still land in the correct position on restore.
        seq.append(c.isValid() ? c.name(QColor::HexArgb) : QString());
    }
    m_config.setTabColorSequence(seq);
}

void MainWindow::applyTabColorSequence() {
    if (!m_coloredTabBar) return;
    const QJsonArray seq = m_config.tabColorSequence();
    const int limit = std::min<int>(seq.size(), m_tabWidget->count());
    for (int i = 0; i < limit; ++i) {
        const QString hex = seq.at(i).toString();
        if (hex.isEmpty()) continue;
        // Only apply if this tab doesn't already have a color (the
        // UUID-keyed path may have beaten us to it when session
        // persistence is on; don't clobber that).
        if (m_coloredTabBar->tabColor(i).isValid()) continue;
        const QColor c(hex);
        if (c.isValid())
            m_coloredTabBar->setTabColor(i, c);
    }
}

void MainWindow::applyPersistedTabColor(QWidget *tabRoot) {
    if (!m_coloredTabBar) return;
    const QString tabId = m_tabSessionIds.value(tabRoot);
    if (tabId.isEmpty()) return;

    const QJsonObject groups = m_config.tabGroups();
    const QString hex = groups.value(tabId).toString();
    if (hex.isEmpty()) return;

    const QColor c(hex);
    if (!c.isValid()) return;

    const int idx = m_tabWidget->indexOf(tabRoot);
    if (idx < 0) return;
    m_coloredTabBar->setTabColor(idx, c);
}

void MainWindow::refreshReviewButton() {
    if (!m_claudeReviewBtn) return;

    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (!t) {
        // No active terminal — button has nothing to review against.
        m_claudeReviewBtn->hide();
        return;
    }

    const QString cwd = t->shellCwd();
    if (cwd.isEmpty()) {
        m_claudeReviewBtn->hide();
        return;
    }

    // Policy (user spec 2026-04-18):
    //   - Not a git repo                      → hide entirely
    //   - Git repo, clean AND in-sync upstream → visible-but-DISABLED
    //     (shows the user "this tab tracks a repo" without advertising
    //     an action there isn't anything to review). The global
    //     QPushButton:hover:enabled CSS gate at mainwindow.cpp:~1850
    //     prevents the disabled button from misleadingly lighting up
    //     on hover.
    //   - Git repo with dirty worktree OR unpushed commits OR no
    //     upstream-but-dirty → visible-AND-enabled, clickable.
    //
    // One-shot composite probe: `git status --porcelain=v1 -b`. Output
    // shape:
    //   ## <branch>...<remote>/<branch> [ahead N, behind M]
    //   M  changed-file
    //   ?? untracked-file
    // The branch header always appears (even on clean repos). Dirty
    // iff any non-header line is present. Ahead iff header carries
    // `ahead`. Combines two probes into one subprocess — cheaper than
    // the previous `git diff --quiet HEAD` which only caught worktree
    // delta and missed unpushed commits.
    auto *proc = new QProcess(this);
    proc->setWorkingDirectory(cwd);
    proc->setProgram("git");
    proc->setArguments({"status", "--porcelain=v1", "-b"});

    QPointer<QPushButton> btn = m_claudeReviewBtn;
    QPointer<QProcess> guard = proc;
    connect(proc, &QProcess::finished, this,
            [btn, guard](int exitCode, QProcess::ExitStatus status) {
        if (btn) {
            if (status != QProcess::NormalExit || exitCode == 128) {
                btn->hide();           // not a git repo / git crash
            } else if (exitCode != 0) {
                btn->hide();
            } else {
                const QByteArray raw = guard ? guard->readAllStandardOutput()
                                              : QByteArray();
                const QList<QByteArray> lines = raw.split('\n');
                bool dirty = false;
                bool ahead = false;
                for (const QByteArray &ln : lines) {
                    if (ln.isEmpty()) continue;
                    if (ln.startsWith("##")) {
                        // Branch header. "ahead N" means local has
                        // unpushed commits; "behind N" alone does not
                        // indicate reviewable local work (nothing to
                        // push), so we ignore it for the enabled state.
                        if (ln.contains("[ahead ") || ln.contains(", ahead "))
                            ahead = true;
                    } else {
                        dirty = true;
                    }
                }
                btn->setEnabled(dirty || ahead);
                btn->show();
            }
        }
        if (guard) guard->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this,
            [btn, guard](QProcess::ProcessError) {
        if (btn) btn->hide();
        if (guard) guard->deleteLater();
    });
    proc->start();
}

void MainWindow::showDiffViewer() {
    // Diagnostic — confirms the click handler fired.
    showStatusMessage(QStringLiteral("Review Changes: opening…"), 1500);

    auto *t = focusedTerminal();
    if (!t) {
        showStatusMessage("Review Changes: no active terminal", 4000);
        return;
    }
    QString cwd = t->shellCwd();
    if (cwd.isEmpty()) {
        showStatusMessage("Review Changes: could not determine working directory "
                          "(shell may not be running yet)", 4000);
        return;
    }

    // Build the dialog BEFORE running any git command. Plain
    // Qt::Dialog flags — no modality (ApplicationModal was causing
    // flaky first-open on KWin when combined with the frameless
    // parent + focus-redirect lambda; the async git probes were
    // fine, but Qt's modal state transition doesn't compose well
    // with our chrome-auto-refocus path). Instead of Qt-side
    // modality, we DISABLE the Review Changes button until the
    // dialog closes — simpler, no Qt modal state machine, and
    // satisfies the user spec "preventing further clicks until
    // the dialog is closed" at the binding site (the button).
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("reviewChangesDialog"));
    dialog->setWindowTitle("Review Changes");
    dialog->resize(800, 600);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    // Block re-entry via the button. Re-enable when the dialog is
    // destroyed (WA_DeleteOnClose → destroyed signal fires after
    // the widget tears down). Using destroyed() rather than
    // finished()/closeEvent lets us catch all close paths —
    // window-manager X button, Escape, Alt-F4, Close button —
    // without having to wire each one individually.
    if (m_claudeReviewBtn) m_claudeReviewBtn->setEnabled(false);
    QPointer<QPushButton> reviewBtnGuard(m_claudeReviewBtn);
    connect(dialog, &QObject::destroyed, this, [this, reviewBtnGuard]() {
        if (reviewBtnGuard) {
            reviewBtnGuard->setEnabled(true);
        }
        // Re-run refreshReviewButton so the enabled state reflects
        // the current git state (may have flipped during the time
        // the dialog was open).
        refreshReviewButton();
    });

    auto *layout = new QVBoxLayout(dialog);
    auto *viewer = new QTextEdit(dialog);
    viewer->setReadOnly(true);
    viewer->setFont(QFont("Monospace", 10));

    auto *btnBox = new QHBoxLayout;
    auto *closeBtn = new QPushButton("Close", dialog);
    auto *copyBtn = new QPushButton("Copy Diff", dialog);
    btnBox->addStretch();
    btnBox->addWidget(copyBtn);
    btnBox->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);

    layout->addWidget(viewer);
    layout->addLayout(btnBox);

    // Show NOW. No git has run yet; the viewer carries a loading
    // placeholder so the dialog isn't empty on first paint. raise()
    // + activateWindow() bring it above the frameless parent; the
    // WindowStaysOnTopHint flag already set in the flags keeps it
    // there regardless of KWin stacking heuristics.
    const Theme &th = Themes::byName(m_currentTheme);
    viewer->setHtml(QStringLiteral(
        "<pre style='color: %1; background: %2;'>"
        "<span style='color: %3;'>Loading git status, diff, and "
        "unpushed commits for:</span>\n  %4\n\n"
        "<span style='color: %3;'>(Running `git status`, `git diff "
        "HEAD`, and `git log @{u}..HEAD` in the background…)</span>"
        "</pre>")
        .arg(th.textPrimary.name(), th.bgPrimary.name(),
             th.textSecondary.name(), cwd.toHtmlEscaped()));
    dialog->show();
    dialog->raise();
    dialog->activateWindow();

    // Shared state for the three async probes. When all three
    // finish (or error out), we rebuild the viewer HTML with the
    // real data and re-install the copy handler. std::shared_ptr
    // because lambdas outlive any single QProcess callback.
    struct ProbeState {
        QString cwd;
        QString status;
        QString diff;
        QString unpushed;
        int pending = 3;
    };
    auto state = std::make_shared<ProbeState>();
    state->cwd = cwd;

    QPointer<QDialog> dlgGuard(dialog);
    QPointer<QTextEdit> viewerGuard(viewer);
    QPointer<QPushButton> copyGuard(copyBtn);
    const QString themeName = m_currentTheme;

    // Finalizer: called once per probe. When pending hits 0, render
    // the full HTML.
    auto finalize = [state, dlgGuard, viewerGuard, copyGuard, themeName]() {
        if (--state->pending > 0) return;
        if (!dlgGuard || !viewerGuard) return;

        const Theme &th = Themes::byName(themeName);
        QString html = QStringLiteral("<pre style='color: %1; background: %2;'>")
                           .arg(th.textPrimary.name(), th.bgPrimary.name());
        auto section = [&html, &th](const QString &title) {
            html += QStringLiteral("<span style='color: %1; font-weight:600;'>"
                                  "━━ %2 ━━</span>\n")
                        .arg(th.ansi[6].name(), title.toHtmlEscaped());
        };
        if (!state->status.isEmpty()) {
            section(QStringLiteral("Status"));
            for (const QString &line : state->status.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith("##"))
                    html += QStringLiteral("<span style='color: %1;'>")
                                .arg(th.ansi[4].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
            html += "\n";
        }
        if (!state->unpushed.isEmpty()) {
            section(QStringLiteral("Unpushed commits"));
            for (const QString &line : state->unpushed.split('\n'))
                html += QStringLiteral("<span style='color: %1;'>")
                            .arg(th.ansi[3].name())
                     + line.toHtmlEscaped() + "</span>\n";
            html += "\n";
        }
        if (!state->diff.isEmpty()) {
            section(QStringLiteral("Diff"));
            for (const QString &line : state->diff.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith('+') && !line.startsWith("+++"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[2].name()) + esc + "</span>\n";
                else if (line.startsWith('-') && !line.startsWith("---"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[1].name()) + esc + "</span>\n";
                else if (line.startsWith("@@"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[4].name()) + esc + "</span>\n";
                else if (line.startsWith("diff ") || line.startsWith("index "))
                    html += QStringLiteral("<span style='color: %1;'>").arg(th.ansi[3].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
        }
        if (state->status.isEmpty() && state->diff.isEmpty() && state->unpushed.isEmpty()) {
            html += QStringLiteral(
                "<span style='color: %1;'>No status, diff, or unpushed commits "
                "to report.</span>\n"
                "<span style='color: %2;'>If you expected changes here, check "
                "that `git status` works in this directory:\n  %3</span>\n")
                .arg(th.ansi[3].name(), th.textSecondary.name(),
                     state->cwd.toHtmlEscaped());
        }
        html += "</pre>";
        viewerGuard->setHtml(html);

        if (copyGuard) {
            // Wire the Copy button now that the data is known. Disconnect
            // first so we don't stack handlers if finalize were ever
            // invoked twice (shouldn't happen, but cheap insurance).
            copyGuard->disconnect();
            QObject::connect(copyGuard, &QPushButton::clicked, copyGuard, [state]() {
                QString combined;
                if (!state->status.isEmpty())
                    combined += "# Status\n" + state->status + "\n\n";
                if (!state->unpushed.isEmpty())
                    combined += "# Unpushed\n" + state->unpushed + "\n\n";
                if (!state->diff.isEmpty())
                    combined += "# Diff\n" + state->diff;
                QApplication::clipboard()->setText(combined);
            });
        }
    };

    // Spawn one async QProcess per probe. Each one writes into its
    // slot on the shared ProbeState when it finishes, then calls
    // finalize(). No blocking on the UI thread.
    auto runAsync = [this, cwd, finalize](const QStringList &args,
                                           QString ProbeState::*slot,
                                           ProbeState *st) {
        auto *p = new QProcess(this);
        p->setWorkingDirectory(cwd);
        p->setProgram("git");
        p->setArguments(args);
        QPointer<QProcess> pg = p;
        auto st_ptr = st;  // raw — lifetime is held by the shared_ptr
                           // captured through `finalize`
        QObject::connect(p,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [st_ptr, slot, pg, finalize](int /*code*/, QProcess::ExitStatus /*es*/) {
                if (pg) st_ptr->*slot = QString::fromUtf8(pg->readAllStandardOutput()).trimmed();
                if (pg) pg->deleteLater();
                finalize();
            });
        QObject::connect(p, &QProcess::errorOccurred, this,
            [pg, finalize](QProcess::ProcessError) {
                if (pg) pg->deleteLater();
                finalize();
            });
        p->start();
    };

    runAsync({"status", "-b", "--short"},                       &ProbeState::status,   state.get());
    runAsync({"diff", "--stat", "--patch", "HEAD"},              &ProbeState::diff,     state.get());
    runAsync({"log", "--oneline", "--decorate", "@{u}..HEAD"},   &ProbeState::unpushed, state.get());
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
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
#endif
    // Pre-Qt-6.5: slot is wired only above the version guard, so this body
    // is unreachable on those builds. Keeping the signature available in
    // both branches avoids a header version check too.
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

    // 0.6.28 — cache compiled regexes across the 2 s poll tick. The old
    // code compiled QRegularExpression(pattern) on every rule on every
    // tick; 10 rules × 1 focused-terminal × 30 ticks/min = 300 JIT
    // compiles/min for no reason, because patterns almost never change
    // between ticks. Cache is a function-local static keyed on the raw
    // pattern string — patterns retired from config stay in cache but
    // that's a few bytes apiece. The `warned` set holds patterns we've
    // already logged as invalid so the status line doesn't flood on
    // every tick with the same regex syntax error.
    static QHash<QString, QRegularExpression> s_patternCache;
    static QSet<QString> s_warnedInvalid;

    for (const QJsonValue &rv : rules) {
        QJsonObject rule = rv.toObject();
        QString pattern = rule.value("pattern").toString();
        QString type = rule.value("type").toString("title");
        QString profileName = rule.value("profile").toString();

        if (pattern.isEmpty() || profileName.isEmpty()) continue;
        if (!profiles.contains(profileName)) continue;

        auto it = s_patternCache.find(pattern);
        if (it == s_patternCache.end()) {
            QRegularExpression compiled(pattern);
            if (!compiled.isValid()) {
                // Warn once per invalid pattern, then drop the rule
                // silently for future ticks until the pattern is edited
                // to something valid (which would create a new cache key).
                if (!s_warnedInvalid.contains(pattern)) {
                    s_warnedInvalid.insert(pattern);
                    showStatusMessage(
                        QStringLiteral("Auto-profile rule skipped — invalid regex: %1")
                            .arg(compiled.errorString()),
                        5000);
                }
                continue;
            }
            it = s_patternCache.insert(pattern, compiled);
        }
        const QRegularExpression &rx = it.value();
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

// --- Command palette rebuild + plugin entries (0.6.9) ---

void MainWindow::rebuildCommandPalette() {
    if (!m_commandPalette) return;
    QList<QAction *> all;
    for (QAction *menuAction : m_menuBar->actions()) {
        if (menuAction->menu())
            collectActions(menuAction->menu(), all);
    }
#ifdef ANTS_LUA_PLUGINS
    // Append plugin-registered entries last so they sort below built-ins —
    // keeps muscle memory for users who already know the menu hierarchy.
    for (const auto &e : m_pluginPaletteEntries) {
        if (e.qaction) all.append(e.qaction);
    }
#endif
    m_commandPalette->setActions(all);
}

#ifdef ANTS_LUA_PLUGINS
void MainWindow::onPluginPaletteRegistered(const QString &pluginName,
                                            const QString &title,
                                            const QString &action,
                                            const QString &hotkey) {
    // Defensive de-dup: a single plugin re-registering the same (title, action)
    // tuple replaces the prior entry rather than stacking a duplicate. Common
    // when init.lua runs more than once during a hot-reload race.
    for (int i = 0; i < m_pluginPaletteEntries.size(); ++i) {
        const auto &e = m_pluginPaletteEntries[i];
        if (e.plugin == pluginName && e.title == title && e.action == action) {
            if (e.qaction)  e.qaction->deleteLater();
            if (e.shortcut) e.shortcut->deleteLater();
            m_pluginPaletteEntries.removeAt(i);
            break;
        }
    }

    PluginPaletteEntry entry;
    entry.plugin = pluginName;
    entry.title  = title;
    entry.action = action;
    entry.hotkey = hotkey;

    // Visible label: "<plugin>: <title>" so the palette stays scannable when
    // multiple plugins contribute entries with similar names.
    QString label = QString("%1: %2").arg(pluginName, title);
    entry.qaction = new QAction(label, this);
    if (!hotkey.isEmpty()) {
        QKeySequence ks(hotkey);
        if (!ks.isEmpty()) entry.qaction->setShortcut(ks);
    }
    QString plugin = pluginName;  // capture by value
    QString actionId = action;
    connect(entry.qaction, &QAction::triggered, this, [this, plugin, actionId]() {
        if (m_pluginManager) m_pluginManager->firePaletteAction(plugin, actionId);
    });

    // Optional standalone QShortcut so the hotkey works even when the palette
    // isn't open. Mirrors the manifest "keybindings" mechanism — registered
    // here per-entry so plugin authors can choose the entry-vs-keybinding
    // scope (palette only vs always-active).
    if (!hotkey.isEmpty()) {
        QKeySequence ks(hotkey);
        if (!ks.isEmpty()) {
            entry.shortcut = new QShortcut(ks, this);
            connect(entry.shortcut, &QShortcut::activated, this,
                    [this, plugin, actionId]() {
                if (m_pluginManager) m_pluginManager->firePaletteAction(plugin, actionId);
            });
        }
    }

    m_pluginPaletteEntries.append(entry);
    rebuildCommandPalette();
}

void MainWindow::clearPluginPaletteEntriesFor(const QString &pluginName) {
    for (int i = m_pluginPaletteEntries.size() - 1; i >= 0; --i) {
        if (m_pluginPaletteEntries[i].plugin != pluginName) continue;
        if (m_pluginPaletteEntries[i].qaction)  m_pluginPaletteEntries[i].qaction->deleteLater();
        if (m_pluginPaletteEntries[i].shortcut) m_pluginPaletteEntries[i].shortcut->deleteLater();
        m_pluginPaletteEntries.removeAt(i);
    }
    rebuildCommandPalette();
}
#endif
