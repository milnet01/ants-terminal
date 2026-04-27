#include "mainwindow.h"

#include "coloredtabbar.h"
#include "opaquemenubar.h"
#include "opaquestatusbar.h"
#include "terminalwidget.h"
#include "titlebar.h"
#include "commandpalette.h"
#include "aidialog.h"
#include "sshdialog.h"
#include "settingsdialog.h"
#include "sessionmanager.h"
#include "remotecontrol.h"
#include "xcbpositiontracker.h"
#include "claudeallowlist.h"
#include "claudebgtasks.h"
#include "claudebgtasksdialog.h"
#include "roadmapdialog.h"
#include "claudeintegration.h"
#include "claudetabtracker.h"
#include "claudeprojects.h"
#include "claudetranscript.h"
#include "auditdialog.h"
#include "shellutils.h"
#include "elidedlabel.h"
#include "globalshortcutsportal.h"
#include "debuglog.h"

namespace {
// Forward declaration — definition lives next to setupQuakeMode() (its
// only other caller) so the conversion table is one scroll away from
// the portal binding.
QString qtKeySequenceToPortalTrigger(const QString &qtHotkey);

// Defined below, after the Qt includes — sweeps stale
// `/tmp/kwin_*_ants_*.js` orphans on startup.
void sweepKwinScriptOrphansOnce();
}

#ifdef ANTS_LUA_PLUGINS
#include "pluginmanager.h"
#endif

#include <algorithm>
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QShowEvent>
#include <QMoveEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QFrame>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QStatusBar>
#include <QToolButton>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QProcess>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QTemporaryFile>
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
#include <QWindow>

#ifdef ANTS_WAYLAND_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

namespace {
// Sweep stale `/tmp/kwin_{pos,move,center}_ants_*.js` files. These are
// written by xcbpositiontracker and the mainwindow move/center helpers
// as `QTemporaryFile(autoRemove=false)` + chained-dbus removal on
// script-unload. A crash, SIGKILL, or dbus-send hang between write and
// unload orphans the file. No functional harm — KWin has already loaded
// its copy — but the files accumulate in /tmp. Sweep anything older
// than one hour on startup; that comfortably clears genuine orphans
// without racing an in-flight script that another instance just wrote.
// Runs once per process; a second MainWindow (File → New Window) does
// not re-sweep.
void sweepKwinScriptOrphansOnce() {
    static bool swept = false;
    if (swept) return;
    swept = true;
    QDir tmp(QDir::tempPath());
    const QStringList patterns = {
        QStringLiteral("kwin_pos_ants_*.js"),
        QStringLiteral("kwin_move_ants_*.js"),
        QStringLiteral("kwin_center_ants_*.js"),
    };
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-3600);
    const QFileInfoList stale = tmp.entryInfoList(
        patterns, QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &fi : stale) {
        if (fi.lastModified() < cutoff) {
            QFile::remove(fi.absoluteFilePath());
        }
    }
}
}  // namespace

MainWindow::MainWindow(bool quakeMode, QWidget *parent) : QMainWindow(parent) {
    sweepKwinScriptOrphansOnce();

    // Disable QMainWindow's built-in QWidgetAnimator. It exists to
    // animate dock-widget resizes and rearrangements — we have no
    // dock widgets, and the animator drives a 60 Hz
    // QPropertyAnimation(target=QWidget, prop=geometry) cycle
    // continuously on an idle window (1129 DeferredDelete entries
    // for that animation in an 8 s debug log), which cascades a
    // LayoutRequest → UpdateRequest → full-widget-tree paint every
    // frame and surfaces as visible dropdown flicker when any menu
    // is open. Root cause of the flicker the user reported 2026-04-20
    // and we chased through eight failed fixes before instrumenting
    // the event loop.
    setAnimated(false);
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
    //
    // Diagnostic escape hatch: ANTS_OPAQUE_WINDOW=1 skips the
    // WA_TranslucentBackground call. Used to isolate whether residual
    // popup / menubar / dropdown flicker on KWin + Wayland is a
    // translucent-parent interaction or something else. Trade-off:
    // per-pixel terminal-area transparency (the `opacity` config key)
    // has no effect with this env var set, since the toplevel window
    // is now opaque at the compositor level.
    const bool forceOpaque = qEnvironmentVariableIntValue("ANTS_OPAQUE_WINDOW") != 0;
    if (!forceOpaque) {
        setAttribute(Qt::WA_TranslucentBackground, true);
    }
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

    // Standalone menu bar — uses OpaqueMenuBar (a QMenuBar subclass
    // whose paintEvent unconditionally fillRects the widget rect with
    // the theme's secondary bg color before delegating to QMenuBar's
    // own paint). Why a subclass and not a stack of attributes:
    //
    // The parent window has Qt::WA_TranslucentBackground (per-pixel
    // alpha for the terminal-area opacity feature). Under translucent
    // parents, none of these "make this widget paint opaquely"
    // attributes is reliable on every WM/style stack:
    //   * autoFillBackground is suppressed when WA_OpaquePaintEvent is
    //     set on the same widget.
    //   * QSS `QMenuBar { background-color: … }` is supposed to draw
    //     via QStyleSheetStyle::drawControl(CE_MenuBarEmptyArea), but
    //     on KWin + Breeze + Qt 6 this draw is skipped when
    //     WA_OpaquePaintEvent is set (the QSS engine assumes the
    //     widget owns those pixels).
    //   * QPalette::Window only feeds autoFillBackground, so it
    //     inherits the same suppression.
    //
    // Result before this fix (user report 2026-04-25): the menubar
    // strip rendered the desktop wallpaper through, with every QSS /
    // palette / autoFill safeguard already in place. The paintEvent
    // override in OpaqueMenuBar is the only path that actually keeps
    // the WA_OpaquePaintEvent contract honest under WA_TranslucentBackground.
    //
    // We still set WA_StyledBackground (so QSS sub-rules like
    // ::item:hover are polished on this widget) and WA_OpaquePaintEvent
    // (a hint to Qt's region tracking that suppresses the open-
    // dropdown compositor-damage flicker on KWin —
    // menubar_hover_stylesheet INV-3b). autoFillBackground is left in
    // place for paranoia: if a future Qt version ever stops respecting
    // WA_OpaquePaintEvent's auto-fill suppression, we'll get a second
    // opaque layer for free; if it keeps respecting it (today's
    // behavior), the call is a no-op.
    //
    // setNativeMenuBar(false) is explicit here so DE integrations that
    // try to export the menubar to a global-menu channel (Unity, KDE
    // appmenu dbusmenu) get told "no" — the menubar must render in
    // our frameless window or the File/Edit/View entries disappear.
    m_menuBar = new OpaqueMenuBar(this);
    m_menuBar->setNativeMenuBar(false);
    m_menuBar->setAutoFillBackground(true);
    m_menuBar->setAttribute(Qt::WA_StyledBackground, true);
    m_menuBar->setAttribute(Qt::WA_OpaquePaintEvent, true);

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

    // Tab-bar opaque background: same translucent-parent failure mode
    // as the menubar (see opaquemenubar.h). The fillRect override in
    // ColoredTabBar::paintEvent does the actual painting; setting
    // WA_OpaquePaintEvent + WA_StyledBackground here keeps the QSS
    // sub-rules (::tab, ::tab:selected, ::close-button) polished and
    // hints to Qt's region tracking that the widget owns its pixels,
    // which suppresses dropdown compositor-damage flicker on KWin
    // (mirrors the menubar setup at the m_menuBar construction site).
    // applyTheme() supplies the actual fill colour via setBackgroundFill.
    if (m_coloredTabBar) {
        m_coloredTabBar->setAutoFillBackground(true);
        m_coloredTabBar->setAttribute(Qt::WA_StyledBackground, true);
        m_coloredTabBar->setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

    // Install OpaqueStatusBar before the first statusBar() call. Qt's
    // QMainWindow::statusBar() lazy-creates a plain QStatusBar on first
    // access — once that happens, setStatusBar() replaces it but we'd
    // already have a window of frames during construction painting the
    // wrong (translucent) bar. Installing first guarantees every paint
    // goes through the opaque subclass. Same WA_OpaquePaintEvent /
    // WA_StyledBackground / autoFillBackground belt-and-suspenders as
    // the menubar — the fillRect in OpaqueStatusBar::paintEvent is what
    // actually keeps the bar opaque under WA_TranslucentBackground.
    m_statusBar = new OpaqueStatusBar(this);
    m_statusBar->setAutoFillBackground(true);
    m_statusBar->setAttribute(Qt::WA_StyledBackground, true);
    m_statusBar->setAttribute(Qt::WA_OpaquePaintEvent, true);
    setStatusBar(m_statusBar);

    setupMenus();

    // Install the app-wide event filter once — it's cheap when the
    // DebugLog bit-test at the top of eventFilter() is false. Menu-
    // scoped install for the intra-action mouse-move suppression
    // happens later.
    qApp->installEventFilter(this);

    // Dropdown-flicker kill-switch: when any QMenu owned by the
    // menubar is about to show, install a global event filter on
    // QApplication; remove it on hide. The filter swallows MouseMove
    // events whose global position lands over the menubar action
    // that OWNS the currently-open popup (intra-action motion).
    // Cross-item motion (File → Edit switch) is passed through so
    // QMenuBar can still switch menus.
    //
    // Why app-level: when a QMenu opens via popup() it grabs the
    // mouse globally. Every subsequent MouseMove event is delivered
    // to the QMenu first (not to QMenuBar), so a filter installed
    // on m_menuBar alone never sees them. A filter on qApp runs
    // before QMenu::event() and can drop the event before QMenu's
    // internal hover tracking schedules a repaint — which is the
    // actual source of the flicker the user sees over the dropdown
    // (2026-04-20 report; survived stylesheet, menubar-attribute,
    // and per-menu-attribute fixes).
    // The previous iteration here set WA_NoSystemBackground,
    // WA_OpaquePaintEvent, and autoFillBackground on each dropdown
    // QMenu, plus an event-filter install / menubar setUpdatesEnabled
    // dance on aboutToShow/aboutToHide. That was chasing a symptom:
    // each attribute changed the menubar's background appearance
    // (theme drift the user flagged) without actually fixing the
    // dropdown flicker. Root cause was upstream — QOpenGLWidget's
    // default NoPartialUpdate mode forcing full-window repaints on
    // every terminal paint. Fixed in terminalwidget.cpp by switching
    // to QOpenGLWidget::PartialUpdate. With that fix, the per-menu
    // attribute hacks aren't needed and would only interfere with
    // theme propagation, so they're gone.

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

    // Main-thread stall detector (ROADMAP § 0.8.0 "Terminal throughput
    // slowdowns" — user report 2026-04-20: "slow down experienced at
    // various times; when tab has been clear or has had lots of text").
    // A 200 ms heartbeat on the event loop. Each firing compares the
    // wall-clock gap since the previous firing to the scheduled
    // interval. Drift above `kStallThresholdMs` means the loop was
    // blocked by some handler (paint, timer, signal slot, Lua
    // callback, synchronous I/O) for that long — exactly the
    // signature of the "intermittent slowdown" the user feels.
    //
    // Gated by the `perf` debug category so the timer is only armed
    // when ANTS_DEBUG=perf (or "all") is set, and even then the log
    // is only written on threshold breach — zero output under normal
    // operation, concrete stall sites on reproduction.
    if (DebugLog::enabled(DebugLog::Perf)) {
        m_stallTimer = new QTimer(this);
        m_stallTimer->setInterval(200);
        m_stallLastFire.start();
        connect(m_stallTimer, &QTimer::timeout, this, [this]() {
            constexpr qint64 kInterval = 200;
            constexpr qint64 kStallThresholdMs = 100;  // report drift > 100 ms
            const qint64 gap = m_stallLastFire.restart();
            const qint64 drift = gap - kInterval;
            if (drift > kStallThresholdMs) {
                ++m_stallCount;
                if (drift > m_stallWorstMs) m_stallWorstMs = drift;
                ANTS_LOG(DebugLog::Perf,
                    "STALL: main-thread blocked for %lldms "
                    "(gap=%lldms, interval=%lldms, count=%llu, worst=%lldms)",
                    static_cast<long long>(drift),
                    static_cast<long long>(gap),
                    static_cast<long long>(kInterval),
                    static_cast<unsigned long long>(m_stallCount),
                    static_cast<long long>(m_stallWorstMs));
            }
        });
        m_stallTimer->start();
        ANTS_LOG(DebugLog::Perf,
            "stall detector armed: interval=200ms threshold=100ms");
    }

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

        // Any visible top-level QDialog blocks the redirect, whether
        // modal or not. Reason: QMessageBox::exec() / QDialog::exec()
        // sets modality inside a brief handshake — activeModalWidget()
        // can return null for a tick during show(). If a focusChanged
        // event fires in that window (e.g. initial default-button focus,
        // or a mid-click focus bounce), the redirect would steal the
        // click from the dialog button. Symptom 2026-04-19: the paste-
        // confirmation dialog's "Paste" button swallowed mouse clicks —
        // only the &Paste keyboard shortcut worked. Walking the top-
        // level widget list catches the dialog regardless of exec()'s
        // modality state.
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w == this) continue;
            if (!w->isVisible()) continue;
            if (w->inherits("QDialog")) return;
        }

        // A popup (QMenu, combobox dropdown, tooltip) is currently open.
        // Users navigate popups by moving the mouse across items; Qt
        // synthesizes focus churn as they do. If we redirect focus back
        // to the terminal mid-navigation, the menubar highlight is wiped
        // on every paint tick — visible as the File/Edit/View hover
        // flashing the user reported 2026-04-19.
        if (QApplication::activePopupWidget()) return;

        // Same reasoning for the menubar itself (which is NOT a popup —
        // it's a regular child widget, so activePopupWidget() misses it).
        // Hovering across menubar actions can briefly park focus on a
        // chrome widget between entering one action and the next. When
        // the cursor is over a menu or menubar, the user is engaging with
        // it; leave focus wherever it wants to go until they move off.
        if (QWidget *under = QApplication::widgetAt(QCursor::pos())) {
            for (QWidget *w = under; w; w = w->parentWidget()) {
                if (w->inherits("QMenu") || w->inherits("QMenuBar")) return;
            }
        }

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
                // Re-check popup + menu-hover at fire time: a menu may
                // have opened between queue and fire (e.g. user clicked
                // File right after focus bounced through chrome). Same
                // reasoning as the queue-time guards — don't yank focus
                // while the user is engaging with a menu.
                if (QApplication::activePopupWidget()) return;
                if (QWidget *under = QApplication::widgetAt(QCursor::pos())) {
                    for (QWidget *w = under; w; w = w->parentWidget()) {
                        if (w->inherits("QMenu") || w->inherits("QMenuBar")) return;
                    }
                }

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
    if (quakeMode || m_config.quakeMode()) {
        setupQuakeMode();

        // Two-path activation: an in-app QShortcut that fires when Ants
        // has focus, plus a Freedesktop Portal GlobalShortcuts binding
        // that fires whether or not Ants has focus (0.6.39). The in-app
        // path from 0.6.38 stays as the always-on fallback because the
        // portal is only implemented by some backends (KDE Plasma 6,
        // xdg-desktop-portal-hyprland, -wlr) — GNOME Shell and the
        // X11-on-legacy-portal cases fall back to the in-app binding.
        //
        // Double-fire debounce: on focused systems where both paths
        // deliver the same key press, we'd hide-then-show (visible
        // flicker). The in-app lambda and the portal lambda both stamp
        // m_lastQuakeToggleMs and reject if the previous stamp is less
        // than 500 ms old. QShortcut is in-process and fires first; the
        // portal's D-Bus round-trip makes its event arrive second, so
        // the debounce drops the portal's duplicate.
        QString hotkeyStr = m_config.quakeHotkey();
        if (!hotkeyStr.isEmpty()) {
            QKeySequence hotkey(hotkeyStr);
            if (!hotkey.isEmpty()) {
                auto *sc = new QShortcut(hotkey, this);
                sc->setContext(Qt::ApplicationShortcut);
                connect(sc, &QShortcut::activated, this, [this]() {
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - m_lastQuakeToggleMs < 500) return;
                    m_lastQuakeToggleMs = now;
                    toggleQuakeVisibility();
                });
            }
        }

        // Portal binding (only when xdg-desktop-portal is on the bus).
        // Request the same hotkey the user configured — the portal's
        // preferred_trigger is advisory, and on first bind KDE's
        // backend shows a system-settings prompt that takes our
        // suggestion as the default. Translation from Qt's
        // "Ctrl+Shift+`" to the portal's "CTRL+SHIFT+grave" is
        // best-effort; unrecognised keys pass through unchanged and
        // the user adjusts in System Settings if needed.
        if (!hotkeyStr.isEmpty() && GlobalShortcutsPortal::isAvailable()) {
            m_gsPortal = new GlobalShortcutsPortal(this);
            connect(m_gsPortal, &GlobalShortcutsPortal::activated, this,
                    [this](const QString &id) {
                        if (id != QStringLiteral("toggle-quake")) return;
                        const qint64 now = QDateTime::currentMSecsSinceEpoch();
                        if (now - m_lastQuakeToggleMs < 500) return;
                        m_lastQuakeToggleMs = now;
                        toggleQuakeVisibility();
                    });
            m_gsPortal->bindShortcut(
                QStringLiteral("toggle-quake"),
                tr("Toggle Ants Terminal drop-down"),
                qtKeySequenceToPortalTrigger(hotkeyStr));
        }
    }

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

    // Remote-control server (first slice of the 0.8.0 Kitty-style
    // rc_protocol item). Listens on
    // `$ANTS_REMOTE_SOCKET` / `$XDG_RUNTIME_DIR/ants-terminal.sock`
    // and currently handles only `{"cmd":"ls"}`; the socket +
    // envelope infrastructure is in place for the next commands to
    // land one-by-one. Failure to bind (another Ants instance
    // already owns the socket) is non-fatal: the log notes it and
    // the main window boots normally.
    m_remoteControl = new RemoteControl(this, this);
    // Gated by config: any process under the user's UID can otherwise
    // drive the terminal via the rc socket (including send-text
    // keystroke injection). Opt-in per 0.7.12 /indie-review finding.
    //
    // The gate snapshots once per process. A second MainWindow (File →
    // New Window) that reads the config after the user toggles the key
    // would otherwise try to bind the same socket and fail — the stale
    // first-window listener (or its absence) is what actually governs
    // accessibility. Cache the first-seen value so the "requires
    // restart" comment is honest for multi-window sessions too.
    static const bool remoteControlGate = m_config.remoteControlEnabled();
    if (remoteControlGate) {
        m_remoteControl->start();
    }
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
        m_sshDialog->setControlMaster(m_config.sshControlMaster());
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

    toolsMenu->addSeparator();

    // Tools → Debug Mode submenu. Each category is a checkable
    // action; ticking one starts writing that category's events to
    // `~/.local/share/ants-terminal/debug.log`. Bottom of submenu
    // has All / None / Open Log File / Clear Log.
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
    };
    for (const auto &entry : catList) {
        QAction *a = debugMenu->addAction(entry.second);
        a->setCheckable(true);
        a->setChecked((DebugLog::active() & entry.first) != 0);
        const quint32 bit = entry.first;
        connect(a, &QAction::toggled, this, [bit](bool on) {
            quint32 cur = DebugLog::active();
            if (on) cur |= bit; else cur &= ~bit;
            DebugLog::setActive(cur);
        });
    }
    debugMenu->addSeparator();
    QAction *debugAllAction = debugMenu->addAction("Enable &All Categories");
    connect(debugAllAction, &QAction::triggered, this, [debugMenu]() {
        DebugLog::setActive(DebugLog::All);
        for (QAction *a : debugMenu->actions())
            if (a->isCheckable()) a->setChecked(true);
    });
    QAction *debugNoneAction = debugMenu->addAction("Disable All (&Off)");
    connect(debugNoneAction, &QAction::triggered, this, [debugMenu]() {
        DebugLog::setActive(DebugLog::None);
        for (QAction *a : debugMenu->actions())
            if (a->isCheckable()) a->setChecked(false);
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

    // --- Help menu ---
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
        const QString qtVer = QString::fromLatin1(qVersion());
        QString luaLine;
#ifdef ANTS_LUA_PLUGINS
        // lua.h is not included here (Lua is a plugin-layer detail).
        // Ship the engine version as a short literal kept in sync with
        // the CMake probe (`Lua ${LUA_VERSION}` line in the configure
        // output). Matches what PluginManager advertises.
        luaLine = QStringLiteral("<br/><b>Lua:</b> 5.4");
#endif
        const QString body = QStringLiteral(
            "<h3>Ants Terminal</h3>"
            "<p><b>Version:</b> %1<br/>"
            "<b>Qt runtime:</b> %2%3</p>"
            "<p>A modern, themeable terminal emulator with GPU "
            "rendering and Lua plugins. MIT-licensed.</p>"
            "<p><a href=\"https://github.com/milnet01/ants-terminal\">"
            "https://github.com/milnet01/ants-terminal</a></p>")
            .arg(QString::fromLatin1(ANTS_VERSION), qtVer, luaLine);

        // Custom QDialog rather than QMessageBox. The previous
        // QMessageBox::Ok variant with Qt::TextBrowserInteraction had a
        // user-reported bug (2026-04-25) where the OK button silently
        // did nothing under our frameless + WA_TranslucentBackground
        // MainWindow on KDE/KWin + Qt 6.11 — the dialog had to be
        // dismissed via the window-manager close button. Switching to
        // QDialog + QDialogButtonBox::Ok with an explicit accepted →
        // accept connection gives us a click path that's standard,
        // testable, and doesn't depend on QMessageBox's internal
        // standard-button dispatch. As a bonus, setOpenExternalLinks
        // on the body label makes the GitHub link actually open in
        // the user's browser (the previous QMessageBox path enabled
        // link-clicking via TextBrowserInteraction but never wired
        // setOpenExternalLinks, so the link click was a no-op too).
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("About Ants Terminal"));
        dlg.setObjectName(QStringLiteral("aboutAntsDialog"));
        auto *layout = new QVBoxLayout(&dlg);
        auto *label = new QLabel(body, &dlg);
        label->setObjectName(QStringLiteral("aboutAntsBody"));
        label->setTextFormat(Qt::RichText);
        // LinksAccessibleByMouse + LinksAccessibleByKeyboard only — no
        // TextSelectableByMouse, so the label doesn't grab focus or
        // intercept mouse events meant for the dialog frame / button.
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                       | Qt::LinksAccessibleByKeyboard);
        label->setOpenExternalLinks(true);
        label->setWordWrap(true);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
        buttons->setObjectName(QStringLiteral("aboutAntsButtons"));
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        layout->addWidget(label);
        layout->addWidget(buttons);
        dlg.exec();
    });

    QAction *aboutQtAction = helpMenu->addAction("About &Qt...");
    connect(aboutQtAction, &QAction::triggered, this, [this]() {
        QMessageBox::aboutQt(this, QStringLiteral("About Qt"));
    });

    helpMenu->addSeparator();
    // 0.7.47 — manual update check. The startup probe already runs
    // 5 s after launch (see m_updateAvailableLabel wiring); this
    // gives the user a way to re-check on demand without restarting.
    QAction *checkUpdatesAction = helpMenu->addAction(tr("Check for &Updates"));
    checkUpdatesAction->setObjectName(
        QStringLiteral("helpCheckForUpdatesAction"));
    connect(checkUpdatesAction, &QAction::triggered, this, [this]() {
        showStatusMessage(tr("Checking for updates…"), 2000);
        checkForUpdates(/*userInitiated=*/true);
    });
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
    terminal->setVisualBell(m_config.visualBell());
    terminal->setPadding(m_config.terminalPadding());
    terminal->setShowCommandMarks(m_config.showCommandMarks());
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
            QWidget *tabRoot = m_tabWidget->widget(i);
            if (tabRoot->isAncestorOf(terminal) || tabRoot == terminal) {
                // Skip if rc_protocol set-title pinned this tab —
                // the user/script chose a label and the shell's OSC
                // 0/2 must not stomp it.
                if (m_tabTitlePins.contains(tabRoot)) break;
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

        // Scroll-scan permission detection always belongs to the terminal
        // whose scrollback was scanned — `terminal` here is a direct
        // pointer, so we capture its shell PID and flag that tab's
        // tracker entry as awaiting input. No session_id routing needed
        // (unlike the hook path); the terminal pointer IS the route.
        pid_t scrollScanAwaitingPid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (scrollScanAwaitingPid > 0)
            m_claudeTabTracker->markShellAwaitingInput(scrollScanAwaitingPid, true);

        auto clearPromptActive = [this, scrollScanAwaitingPid]() {
            m_claudePromptActive = false;
            applyClaudeStatusLabel();
            if (m_claudeTabTracker && scrollScanAwaitingPid > 0)
                m_claudeTabTracker->markShellAwaitingInput(scrollScanAwaitingPid, false);
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

    if (!terminal->startShell(inheritCwd, m_config.shellCommand())) {
        showStatusMessage("Failed to start shell!");
    }

    terminal->setFocus();

    // Track shell process for Claude Code integration
    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(terminal->shellPid());
    if (m_claudeTabTracker && terminal->shellPid() > 0)
        m_claudeTabTracker->trackShell(terminal->shellPid());

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

    if (!newTerm->startShell(QString(), m_config.shellCommand())) {
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

    // Release the per-tab Claude tracker entry BEFORE removeTab — once
    // the widget is detached we can't recover its shell PID.
    if (m_claudeTabTracker && term && term->shellPid() > 0)
        m_claudeTabTracker->untrackShell(term->shellPid());

    m_tabSessionIds.remove(w);
    m_tabTitlePins.remove(w);  // free pin alongside session id
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

TerminalWidget *MainWindow::terminalAtTab(int index) const {
    if (index < 0 || index >= m_tabWidget->count()) return nullptr;
    return activeTerminalInTab(m_tabWidget->widget(index));
}

int MainWindow::currentTabIndexForRemote() const {
    return m_tabWidget->currentIndex();
}

bool MainWindow::setTabTitleForRemote(int index, const QString &title) {
    if (index < 0 || index >= m_tabWidget->count()) return false;
    QWidget *w = m_tabWidget->widget(index);
    if (title.isEmpty()) {
        // Clear the pin and refresh immediately. Two cases:
        //   - tabTitleFormat != "title" → updateTabTitles() does the
        //     work for us based on cwd / process.
        //   - tabTitleFormat == "title" → updateTabTitles bails;
        //     we have to restore the most recent shell-provided title
        //     manually, otherwise the pinned label sits there until
        //     the *next* OSC 0/2 fires (which may be never on a
        //     quiet prompt). Pull it from the active terminal's
        //     `shellTitle()` cache (the same value the titleChanged
        //     signal would have set).
        m_tabTitlePins.remove(w);
        updateTabTitles();
        if (m_config.tabTitleFormat() == "title") {
            if (auto *term = activeTerminalInTab(w)) {
                QString shellTitle = term->shellTitle();
                if (shellTitle.isEmpty()) shellTitle = "Shell";
                if (shellTitle.length() > 30)
                    shellTitle = shellTitle.left(27) + "...";
                m_tabWidget->setTabText(index, shellTitle);
            }
        }
    } else {
        // Pin the label. The titleChanged handler and updateTabTitles
        // both check the pin map before calling setTabText, so the
        // value sticks across both the per-shell signal and the 2 s
        // refresh tick. Truncated to the same 30-char ceiling the
        // signal handler uses to avoid the tab strip ballooning.
        m_tabTitlePins[w] = title;
        QString display = title.length() > 30 ? title.left(27) + "..." : title;
        m_tabWidget->setTabText(index, display);
    }
    return true;
}

bool MainWindow::selectTabForRemote(int index) {
    if (index < 0 || index >= m_tabWidget->count()) return false;
    m_tabWidget->setCurrentIndex(index);
    // Refocus the new tab's terminal so follow-up send-text calls
    // without an explicit tab field land on this pane. Without the
    // explicit setFocus the keyboard focus can stay on whatever
    // widget (menubar, search bar, dialog button) owned it at
    // switch-time.
    if (auto *term = activeTerminalInTab(m_tabWidget->widget(index))) {
        term->setFocus();
    }
    return true;
}

int MainWindow::newTabForRemote(const QString &cwd, const QString &command) {
    // Mirror of the newTab() slot but with explicit cwd/command
    // plumbing so rc_protocol `new-tab` doesn't need to round-trip
    // through signals. Returns the index of the created tab so the
    // caller can target it in follow-up commands.
    QString effectiveCwd = cwd;
    if (effectiveCwd.isEmpty()) {
        if (auto *prev = focusedTerminal())
            effectiveCwd = prev->shellCwd();
        else if (auto *fallback = currentTerminal())
            effectiveCwd = fallback->shellCwd();
    }

    auto *terminal = createTerminal();
    connectTerminal(terminal);

    QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);
    m_tabSessionIds[terminal] = tabId;

    if (!terminal->startShell(effectiveCwd, m_config.shellCommand())) {
        showStatusMessage("Failed to start shell!");
    }
    terminal->setFocus();

    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(terminal->shellPid());
    if (m_claudeTabTracker && terminal->shellPid() > 0)
        m_claudeTabTracker->trackShell(terminal->shellPid());

    // Hide tab bar when only one tab (same logic as newTab slot).
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    if (!command.isEmpty()) {
        // 200 ms settle before writing — same timing the SSH-manager
        // wiring uses (onSshConnect) because the shell child needs a
        // moment to finish its init before it can accept input reliably.
        // Use sendToPty (raw bytes) rather than writeCommand: the
        // caller owns the trailing newline, matching send-text
        // semantics. `launch` is the rc command that auto-appends
        // newlines for the convenience case; `new-tab` stays
        // byte-faithful so a script can write partial lines or
        // include control sequences.
        QPointer<TerminalWidget> guard(terminal);
        QByteArray cmdBytes = command.toUtf8();
        QTimer::singleShot(200, this, [guard, cmdBytes]() {
            if (guard) guard->sendToPty(cmdBytes);
        });
    }
    return idx;
}

QJsonArray MainWindow::tabListForRemote() const {
    // One JSON object per tab. `active: true` on exactly the tab that
    // `currentTerminal()` is inside, so a remote-control client can
    // tell which pane receives input by default. `cwd` reads the
    // focused terminal's shell cwd (via OSC 7 or /proc fallback); may
    // be empty when the shell hasn't sent OSC 7 yet and /proc is
    // unavailable (e.g. stale PID after fork).
    QJsonArray tabs;
    const int n = m_tabWidget->count();
    const int current = m_tabWidget->currentIndex();
    for (int i = 0; i < n; ++i) {
        QJsonObject t;
        t["index"] = i;
        t["title"] = m_tabWidget->tabText(i);
        t["active"] = (i == current);
        QString cwd;
        if (auto *term = activeTerminalInTab(m_tabWidget->widget(i))) {
            cwd = term->shellCwd();
        }
        t["cwd"] = cwd;
        tabs.append(t);
    }
    return tabs;
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
    // The `opacity` config key only affects the terminal content area — this is
    // handled in TerminalWidget::paintEvent via m_windowOpacity (the variable
    // name is historical; it drives per-pixel terminal-area fillRect alpha,
    // not Qt's whole-window setWindowOpacity).
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
        // Explicit base ::item rule prevents Qt from falling back to the
        // native style for the non-selected state. Without it, Qt composites
        // native item drawing under the stylesheet's :selected overlay; on
        // hover transitions the native and stylesheet layers race and the
        // highlight appears to flash (user report 2026-04-19). Transparent
        // background + matching padding makes the non-selected state a no-op
        // so the :selected rule is the only visible state change.
        "QMenuBar::item { background-color: transparent; padding: 4px 10px;"
        "  margin: 0; border-radius: 4px; }"
        // :hover and :selected both map to the same highlight. Qt's
        // QStyleSheetStyle treats them as distinct pseudo-states on
        // QMenuBar::item — Breeze / Fusion hover-flash for one frame
        // before :selected engages, and on that frame only :hover
        // styling applies. Mirroring :selected into :hover removes
        // the one-frame gap (closed-menu hover flash, original
        // 2026-04-20 report). The dropdown-flicker-when-both-apply
        // regression that briefly came up mid-0.7.4 is fixed
        // structurally by Qt::WA_OpaquePaintEvent on the menubar
        // widget (see construction site) — not by dropping this
        // rule.
        "QMenuBar::item:hover { background-color: %5; }"
        "QMenuBar::item:selected { background-color: %5; }"
        "QMenuBar::item:pressed { background-color: %5; }"
        "QMenu { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        // The item's :selected background is a rectangle filling the
        // full item rect (no border-radius). Prior to 0.7.4 the base
        // rule had `border-radius: 4px` on the item — rounded
        // corners force Qt to paint the :selected fill with a clip
        // shape, and the rows outside that clip shape (inside the
        // item's bounding rect but outside the rounded pill) get
        // painted with the menu's background. That compositing path
        // shows a visible flash as the "current item" shifts rows
        // on mouse move — reported 2026-04-20 after the paste-
        // dialog fix exposed the leftover flicker. Dropping the
        // radius means the :selected fill is the whole rect and
        // deselect just repaints the same rect to the menu's
        // background color. No compositing, no flash.
        "QMenu::item { padding: 6px 24px 6px 12px; }"
        "QMenu::item:selected { background-color: %5; color: %1; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QStatusBar { background-color: %2; color: %6; border-top: 1px solid %4; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar { background-color: %2; }"
        "QTabBar::tab { background-color: %2; color: %6; padding: 6px 16px;"
        "  border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %5; }"
        "QTabBar::tab:hover { background-color: %1; color: %3; }"
        // 0.7.32 (user feedback 2026-04-25) — the platform-style fallback
        // (0.6.27) still rendered the × hover-only on Fusion / qt6ct /
        // some Plasma color schemes — users couldn't see where to click
        // without first mousing onto the tab. Force-render the glyph via
        // a data-URI SVG that's always visible regardless of platform
        // style. Hover variant uses textPrimary instead of textSecondary
        // for "lifting" feedback and keeps the ansi-red background (%7)
        // for the will-click cue.
        "QTabBar::close-button {"
        "  image: url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'"
        " width='10' height='10' viewBox='0 0 10 10'>"
        "<line x1='2' y1='2' x2='8' y2='8' stroke='%6'"
        " stroke-width='1.5' stroke-linecap='round'/>"
        "<line x1='8' y1='2' x2='2' y2='8' stroke='%6'"
        " stroke-width='1.5' stroke-linecap='round'/></svg>\");"
        "  subcontrol-position: right; margin: 2px; padding: 1px;"
        "  width: 14px; height: 14px; border-radius: 3px; }"
        "QTabBar::close-button:hover {"
        "  image: url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'"
        " width='10' height='10' viewBox='0 0 10 10'>"
        "<line x1='2' y1='2' x2='8' y2='8' stroke='%3'"
        " stroke-width='1.5' stroke-linecap='round'/>"
        "<line x1='8' y1='2' x2='2' y2='8' stroke='%3'"
        " stroke-width='1.5' stroke-linecap='round'/></svg>\");"
        "  background-color: %7; border-radius: 3px; }"
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
          // %3 — textPrimary; appears verbatim in most rules but ALSO
          // as a stroke color in the data-URI SVG for the tab-close
          // glyph hover variant. Pre-encode the leading `#` as %23 so
          // Qt's CSS parser doesn't truncate the URI at the fragment
          // delimiter; the `%23` literal would also collide with
          // QString::arg() placeholder numbering, so we splice it
          // here rather than embed it in the format string.
          QStringLiteral("%23") + theme.textPrimary.name().mid(1),
          theme.border.name(),
          theme.accent.name(),
          // %6 — textSecondary; same SVG-stroke pre-encoding as %3
          // above, used by the default (non-hover) tab close button.
          QStringLiteral("%23") + theme.textSecondary.name().mid(1),
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

    // Menubar: the OpaqueMenuBar subclass guarantees an opaque fill in
    // its paintEvent (the only thing Qt actually honors under
    // WA_TranslucentBackground + WA_OpaquePaintEvent on KWin / Breeze /
    // Qt 6 — see opaquemenubar.h for why every other path silently
    // dropped the background paint, surfacing as the desktop showing
    // through the menubar strip in the user report 2026-04-25).
    //
    // Palette + widget-local QSS are kept as belt-and-suspenders so
    // child widgets the menubar polishes (QToolButton, dropdown
    // arrows on style stacks that use them) inherit the right colors,
    // and so the QMenuBar::item :hover / :selected / :pressed rules
    // are scoped on the menubar itself rather than relying on the
    // top-level cascade reaching it. The fillRect in OpaqueMenuBar
    // is what actually paints the strip.
    if (m_menuBar) {
        m_menuBar->setBackgroundFill(theme.bgSecondary);
        QPalette p = m_menuBar->palette();
        p.setColor(QPalette::Window, theme.bgSecondary);
        p.setColor(QPalette::Base, theme.bgSecondary);
        p.setColor(QPalette::WindowText, theme.textPrimary);
        m_menuBar->setPalette(p);
        m_menuBar->setStyleSheet(QStringLiteral(
            "QMenuBar { background-color: %1; color: %2; "
            "  border-bottom: 1px solid %3; }"
            "QMenuBar::item { background-color: transparent; "
            "  padding: 4px 10px; margin: 0; border-radius: 4px; }"
            "QMenuBar::item:hover { background-color: %4; }"
            "QMenuBar::item:selected { background-color: %4; }"
            "QMenuBar::item:pressed { background-color: %4; }"
        ).arg(theme.bgSecondary.name(),
              theme.textPrimary.name(),
              theme.border.name(),
              theme.accent.name()));
        m_menuBar->update();
    }

    // Tab bar + status bar: same translucent-parent class of bug as the
    // menubar above. The top-level QSS cascade still publishes the
    // QTabBar / QStatusBar background-color rules (so palette-derived
    // sub-elements that DO honor QSS — tabs, embedded labels — pick up
    // the right colour), but the actual bar-strip fill comes from each
    // widget's paintEvent override. setBackgroundFill is what the
    // override reads; without these calls the strip paints transparent
    // and the desktop wallpaper shows through to the right of the last
    // tab and across the entire status bar. User report 2026-04-25.
    if (m_coloredTabBar) {
        m_coloredTabBar->setBackgroundFill(theme.bgSecondary);
        m_coloredTabBar->update();
    }
    if (m_statusBar) {
        m_statusBar->setBackgroundFill(theme.bgSecondary);
        QPalette sp = m_statusBar->palette();
        sp.setColor(QPalette::Window, theme.bgSecondary);
        sp.setColor(QPalette::WindowText, theme.textSecondary);
        m_statusBar->setPalette(sp);
        m_statusBar->update();
    }

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

    // Unpredictable tempfile via QTemporaryFile — 0.7.12 TOCTOU fix.
    // See xcbpositiontracker.cpp for rationale.
    QString scriptPath;
    {
        QTemporaryFile f(QDir::tempPath() + "/kwin_move_ants_XXXXXX.js");
        f.setAutoRemove(false);
        if (!f.open()) return;
        f.write(kwinJs.toUtf8());
        scriptPath = f.fileName();
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

    // Unpredictable tempfile via QTemporaryFile — 0.7.12 TOCTOU fix.
    QString scriptPath;
    {
        QTemporaryFile f(QDir::tempPath() + "/kwin_center_ants_XXXXXX.js");
        f.setAutoRemove(false);
        if (!f.open()) return;
        f.write(kwinJs.toUtf8());
        scriptPath = f.fileName();
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
        // Thread the manual rename pin (if any) so the user's
        // right-click "Rename Tab…" label survives restart. Key is
        // the outer tab widget (may be a QSplitter for split tabs —
        // the pin is stored at tab-widget granularity, not per-pane).
        const QString pinnedTitle = m_tabTitlePins.value(w);
        SessionManager::saveSession(tabId, t->grid(), t->shellCwd(), pinnedTitle);
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

        // Restore scrollback, screen, working directory, and pinned
        // tab title (V3 session files). Pin takes precedence over the
        // shell-derived window title — the whole point of the manual
        // rename is that it sticks until the user un-renames.
        QString savedCwd;
        QString savedPinnedTitle;
        SessionManager::loadSession(tabId, terminal->grid(), &savedCwd,
                                    &savedPinnedTitle);

        const int newTabIdx = m_tabWidget->count() - 1;
        if (!savedPinnedTitle.isEmpty()) {
            // Re-pin via m_tabTitlePins[terminal] so the titleChanged
            // signal handler and the 2 s updateTabTitles tick both
            // honor it. Can't call setTabTitleForRemote here because
            // it resolves the tab by index against the *outer* widget
            // identity, which is still `terminal` at restore time
            // (splits are never persisted), so writing the pin map
            // directly is equivalent and avoids one lookup.
            m_tabTitlePins[terminal] = savedPinnedTitle;
            QString display = savedPinnedTitle.length() > 30
                ? savedPinnedTitle.left(27) + "..."
                : savedPinnedTitle;
            m_tabWidget->setTabText(newTabIdx, display);
        } else {
            // No pin → fall back to the shell-derived window title.
            QString savedTitle = terminal->grid()->windowTitle();
            if (!savedTitle.isEmpty()) {
                if (savedTitle.length() > 30)
                    savedTitle = savedTitle.left(27) + "...";
                m_tabWidget->setTabText(newTabIdx, savedTitle);
            }
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
            if (!tab.terminal->startShell(tab.startDir, m_config.shellCommand()))
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
    const QString statusStyle = QStringLiteral("padding: 0 8px; font-size: 11px;");

    // NotRunning hides both label and the context progress bar regardless
    // of prompt state — no Claude process = nothing to announce.
    if (m_claudeLastState == ClaudeState::NotRunning) {
        m_claudeStatusLabel->hide();
        if (m_claudeContextBar) m_claudeContextBar->hide();
        return;
    }

    QString text;
    ClaudeTabIndicator::Glyph glyph = ClaudeTabIndicator::Glyph::Idle;

    // Status text vocabulary (user spec 2026-04-18):
    //   idle / thinking / prompting / bash / reading a file / planning /
    //   auditing / compacting / etc.
    // Colour comes from the unified Claude state palette
    // (`ClaudeTabIndicator::color`) so the status-bar text matches the
    // per-tab dot one-for-one — see
    // `tests/features/claude_state_dot_palette/spec.md`.
    if (m_claudePromptActive) {
        // Prompt-active overrides the base state — a waiting permission
        // prompt is what the user needs to see.
        text = QStringLiteral("Claude: prompting");
        glyph = ClaudeTabIndicator::Glyph::AwaitingInput;
    } else if (m_claudePlanMode) {
        // Plan mode is a user-selected interaction mode (Shift+Tab),
        // orthogonal to the transcript-derived state. While in plan mode
        // the assistant can think/read but cannot edit or run commands —
        // "planning" is the honest label.
        text = QStringLiteral("Claude: planning");
        glyph = ClaudeTabIndicator::Glyph::Planning;
    } else if (m_claudeAuditing) {
        // Auditing is detected from a recent user message that invoked
        // the /audit skill in the transcript. Lives beside state because
        // the user can audit during tool use, thinking, or idle — the
        // skill's lifecycle is not the same as any single tool.
        text = QStringLiteral("Claude: auditing");
        glyph = ClaudeTabIndicator::Glyph::Auditing;
    } else {
        switch (m_claudeLastState) {
        case ClaudeState::NotRunning:
            return;  // handled above
        case ClaudeState::Idle:
            text = QStringLiteral("Claude: idle");
            glyph = ClaudeTabIndicator::Glyph::Idle;
            break;
        case ClaudeState::Thinking:
            text = QStringLiteral("Claude: thinking");
            glyph = ClaudeTabIndicator::Glyph::Thinking;
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
                glyph = ClaudeTabIndicator::Glyph::Bash;
            } else {
                if (lower == QLatin1String("read")) {
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
                glyph = ClaudeTabIndicator::Glyph::ToolUse;
            }
            break;
        }
        case ClaudeState::Compacting:
            text = QStringLiteral("Claude: compacting");
            glyph = ClaudeTabIndicator::Glyph::Compacting;
            break;
        }
    }

    const QColor color = ClaudeTabIndicator::color(glyph);
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

    // Per-tab activity tracker. Always constructed (its polling cost is
    // trivial: one /proc read per tracked shell every 2 s, zero when
    // no shell has Claude). The user-facing toggle
    // claude_tab_status_indicator gates the glyph rendering at the
    // provider level — flipping it off doesn't destroy the tracker, it
    // just makes the provider return Glyph::None until the toggle
    // flips back on. That way live config reloads take effect on the
    // next paint without any construct/destruct dance.
    m_claudeTabTracker = new ClaudeTabTracker(this);
    {
        // Provider: look up the tab's active terminal, read its shell
        // PID, and translate the tracker's per-shell state into a
        // Glyph. The `awaitingInput` flag short-circuits whatever the
        // transcript parser said because a pending prompt is what the
        // user most needs to notice.
        m_coloredTabBar->setClaudeIndicatorProvider([this](int tabIndex) {
            ClaudeTabIndicator ind;
            if (!m_claudeTabTracker) return ind;
            if (!m_config.claudeTabStatusIndicator()) return ind;  // toggle off
            auto *term = terminalAtTab(tabIndex);
            if (!term) return ind;
            const pid_t pid = term->shellPid();
            if (pid <= 0) return ind;
            const ClaudeTabTracker::ShellState s = m_claudeTabTracker->shellState(pid);
            if (s.awaitingInput) {
                ind.glyph = ClaudeTabIndicator::Glyph::AwaitingInput;
                return ind;
            }
            if (s.planMode && s.state != ClaudeState::NotRunning) {
                ind.glyph = ClaudeTabIndicator::Glyph::Planning;
                return ind;
            }
            if (s.auditing && s.state != ClaudeState::NotRunning) {
                ind.glyph = ClaudeTabIndicator::Glyph::Auditing;
                return ind;
            }
            switch (s.state) {
                case ClaudeState::NotRunning:
                    ind.glyph = ClaudeTabIndicator::Glyph::None; break;
                case ClaudeState::Idle:
                    ind.glyph = ClaudeTabIndicator::Glyph::Idle; break;
                case ClaudeState::Thinking:
                    ind.glyph = ClaudeTabIndicator::Glyph::Thinking; break;
                case ClaudeState::ToolUse:
                    // Bash is the tool with the most user-relevant runtime
                    // (long-running commands, compilations, greps over
                    // large repos) — split it out so the glyph carries
                    // that signal at a glance.
                    ind.glyph = (s.tool == QLatin1String("Bash"))
                        ? ClaudeTabIndicator::Glyph::Bash
                        : ClaudeTabIndicator::Glyph::ToolUse;
                    break;
                case ClaudeState::Compacting:
                    ind.glyph = ClaudeTabIndicator::Glyph::Compacting; break;
            }
            return ind;
        });
        // Repaint the tab bar whenever any shell's state transitions.
        // Cheap — QWidget::update() coalesces to one paint per event
        // loop iteration, and paintEvent only queries the tracker once
        // per tab. Also refresh the hover tooltip for the owning tab so
        // the user can hover-to-disambiguate ("Claude: Bash" vs
        // "Claude: reading a file") without opening the tab.
        connect(m_claudeTabTracker, &ClaudeTabTracker::shellStateChanged,
                this, [this](pid_t shellPid) {
            if (m_coloredTabBar) m_coloredTabBar->update();
            if (!m_tabWidget) return;
            const int n = m_tabWidget->count();
            for (int i = 0; i < n; ++i) {
                auto *term = terminalAtTab(i);
                if (!term || term->shellPid() != shellPid) continue;
                const auto st = m_claudeTabTracker->shellState(shellPid);
                QString tip;
                if (st.awaitingInput) {
                    tip = tr("Claude: awaiting input");
                } else if (st.planMode && st.state != ClaudeState::NotRunning) {
                    tip = tr("Claude: planning");
                } else if (st.auditing && st.state != ClaudeState::NotRunning) {
                    tip = tr("Claude: auditing");
                } else switch (st.state) {
                    case ClaudeState::NotRunning: tip.clear(); break;
                    case ClaudeState::Idle:       tip = tr("Claude: idle"); break;
                    case ClaudeState::Thinking:   tip = tr("Claude: thinking…"); break;
                    case ClaudeState::Compacting: tip = tr("Claude: compacting…"); break;
                    case ClaudeState::ToolUse:
                        tip = st.tool.isEmpty()
                            ? tr("Claude: tool use")
                            : tr("Claude: %1").arg(st.tool);
                        break;
                }
                m_tabWidget->setTabToolTip(i, tip);
                break;
            }
        });
    }

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

    // 0.7.38 — Background tasks button. Sibling to Review Changes; same
    // size/policy contract. Hidden by default; shown only when the
    // per-session tracker reports ≥1 background task in the active
    // Claude Code transcript.
    m_claudeBgTasks = new ClaudeBgTaskTracker(this);
    m_claudeBgTasksBtn = new QPushButton(tr("Background Tasks"), this);
    m_claudeBgTasksBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_claudeBgTasksBtn->hide();
    statusBar()->addPermanentWidget(m_claudeBgTasksBtn);
    connect(m_claudeBgTasksBtn, &QPushButton::clicked,
            this, &MainWindow::showBgTasksDialog);
    connect(m_claudeBgTasks, &ClaudeBgTaskTracker::tasksChanged,
            this, &MainWindow::refreshBgTasksButton);

    // 0.7.39 — Roadmap button. Sibling to Background Tasks; same size/
    // policy contract. Hidden until the active tab's cwd is probed and
    // a ROADMAP.md surfaces. User asked for it to follow the
    // ROADMAP.md-presence convention so terminals running outside any
    // project root pay nothing.
    m_roadmapBtn = new QPushButton(tr("Roadmap"), this);
    m_roadmapBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_roadmapBtn->hide();
    statusBar()->addPermanentWidget(m_roadmapBtn);
    connect(m_roadmapBtn, &QPushButton::clicked,
            this, &MainWindow::showRoadmapDialog);

    // 0.7.45 — Repo visibility badge. Small QLabel showing
    // "Public" / "Private" for the active tab's GitHub repo. Hidden
    // when the cwd isn't a GitHub-backed repo, when `gh` is missing,
    // or when authentication / network fails. Theme-coloured via
    // applyTheme(). Per-tab via refreshStatusBarForActiveTab.
    m_repoVisibilityLabel = new QLabel(this);
    m_repoVisibilityLabel->setObjectName(QStringLiteral("repoVisibilityLabel"));
    m_repoVisibilityLabel->hide();
    statusBar()->addPermanentWidget(m_repoVisibilityLabel);

    // 0.7.45 — Update-available notifier. Clickable QLabel that
    // appears when the latest GitHub release tag is newer than
    // ANTS_VERSION. Click opens the release page. Hidden by default,
    // surfaced by an hourly + on-startup check via QNetworkAccessManager.
    m_updateAvailableLabel = new QLabel(this);
    m_updateAvailableLabel->setObjectName(QStringLiteral("updateAvailableLabel"));
    // 0.7.46 — intercept clicks via linkActivated rather than letting
    // the QLabel auto-open the URL via setOpenExternalLinks. The
    // handler probes for AppImageUpdate / appimageupdatetool and runs
    // an in-place binary update when available, falling back to the
    // browser only when neither tool is installed.
    m_updateAvailableLabel->setOpenExternalLinks(false);
    m_updateAvailableLabel->setTextFormat(Qt::RichText);
    m_updateAvailableLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(m_updateAvailableLabel, &QLabel::linkActivated,
            this, &MainWindow::handleUpdateClicked);
    m_updateAvailableLabel->hide();
    statusBar()->addPermanentWidget(m_updateAvailableLabel);

    // 0.7.47 — startup-only update check (was hourly in 0.7.45-0.7.46;
    // user feedback "An hourly check I think is a bit much. Let's do
    // the check when the terminal is opened and when the user clicked
    // on Help > Check for Updates."). The 5 s singleShot delay keeps
    // the launch path fast and avoids racing the first paint. Manual
    // re-check is wired through the Help menu — see helpMenu setup.
    // Wrapped in a lambda so the default `userInitiated=false` is
    // forwarded — the bare PMF can't be passed to singleShot's
    // 0-arg slot signature.
    QTimer::singleShot(5000, this,
        [this]() { checkForUpdates(/*userInitiated=*/false); });

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

        // Tab-glyph feedback: flag the owning tab's shell as awaiting
        // input so its tab-bar dot turns loud/orange. The hook server
        // is a single UDS shared across every Claude in every tab, so
        // we must route by session_id (captured by ClaudeIntegration
        // before this slot runs) to find the right shell. Fallback
        // to the active tab when the session isn't tracked (e.g. the
        // first prompt before the tracker's poll has noticed the new
        // Claude child) — the bottom-status-bar already shows
        // "Claude: prompting" for the active tab, so the glyph
        // matches that contract when routing fails.
        pid_t awaitingPid = 0;
        if (m_claudeTabTracker) {
            const QString sid = m_claudeIntegration->lastHookSessionId();
            awaitingPid = m_claudeTabTracker->shellForSessionId(sid);
            if (awaitingPid == 0) {
                if (auto *term = currentTerminal()) awaitingPid = term->shellPid();
            }
            if (awaitingPid > 0)
                m_claudeTabTracker->markShellAwaitingInput(awaitingPid, true);
        }

        auto clearPromptActive = [this, awaitingPid]() {
            m_claudePromptActive = false;
            applyClaudeStatusLabel();
            if (m_claudeTabTracker && awaitingPid > 0)
                m_claudeTabTracker->markShellAwaitingInput(awaitingPid, false);
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

        // Resume a specific session
        connect(m_claudeProjects, &ClaudeProjectsDialog::resumeSession,
                this, [this](const QString &projectPath, const QString &sessionId, bool fork) {
            auto *t = focusedTerminal();
            if (!t) return;
            QString cmd = QString("cd %1 && claude --resume %2")
                          .arg(shellQuote(projectPath), sessionId);
            if (fork) cmd += " --fork-session";
            t->writeCommand(cmd);
        });

        // Continue the latest session in a project
        connect(m_claudeProjects, &ClaudeProjectsDialog::continueProject,
                this, [this](const QString &projectPath) {
            auto *t = focusedTerminal();
            if (!t) return;
            t->writeCommand(QString("cd %1 && claude --continue").arg(shellQuote(projectPath)));
        });

        // Start a new session in a project
        connect(m_claudeProjects, &ClaudeProjectsDialog::newSession,
                this, [this](const QString &projectPath) {
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

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    // Route event-filter-observable events into DebugLog when the
    // relevant categories are active. The `enabled()` check is a
    // single bit-test on the hot path.
    if (DebugLog::enabled(DebugLog::Paint) ||
        DebugLog::enabled(DebugLog::Events)) {
        auto t = event->type();
        const char *tname = nullptr;
        DebugLog::Category cat = DebugLog::None;
        if (t == QEvent::Paint)              { tname = "Paint";          cat = DebugLog::Paint; }
        else if (t == QEvent::UpdateRequest) { tname = "UpdateRequest";  cat = DebugLog::Paint; }
        else if (t == QEvent::UpdateLater)   { tname = "UpdateLater";    cat = DebugLog::Paint; }
        else if (t == QEvent::LayoutRequest) { tname = "LayoutRequest";  cat = DebugLog::Paint; }
        else if (t == QEvent::Resize)        { tname = "Resize";         cat = DebugLog::Events; }
        else if (t == QEvent::Timer)         { tname = "Timer";          cat = DebugLog::Events; }
        else if (t == QEvent::DeferredDelete){ tname = "DeferredDelete"; cat = DebugLog::Events; }
        else if (t == QEvent::FocusIn)       { tname = "FocusIn";        cat = DebugLog::Events; }
        else if (t == QEvent::FocusOut)      { tname = "FocusOut";       cat = DebugLog::Events; }
        else if (t == QEvent::ChildPolished && DebugLog::enabled(DebugLog::Events)) {
            // ChildPolished fires AFTER the derived ctor runs, so the
            // metaObject vtable reports the true class — unlike
            // ChildAdded which fires from QObject's ctor when vtable
            // still points at QObject. This is the right hook for
            // detecting QPropertyAnimation creation.
            auto *ce = static_cast<QChildEvent *>(event);
            QObject *child = ce->child();
            if (child) {
                const char *cls = child->metaObject()->className();
                if (std::string(cls).find("Animation") != std::string::npos) {
                    auto *anim = qobject_cast<QPropertyAnimation *>(child);
                    QObject *tgt = anim ? anim->targetObject() : nullptr;
                    const char *tgtCls = tgt ? tgt->metaObject()->className() : "null";
                    QByteArray tgtName = tgt ? tgt->objectName().toUtf8() : QByteArray();
                    QObject *tgtParent = tgt ? tgt->parent() : nullptr;
                    const char *tgtParCls = tgtParent ? tgtParent->metaObject()->className() : "null";
                    QByteArray tgtParName = tgtParent ? tgtParent->objectName().toUtf8() : QByteArray();
                    const char *parCls = watched->metaObject()->className();
                    QByteArray parName = watched->objectName().toUtf8();
                    ANTS_LOG(DebugLog::Events,
                        "AnimCREATED cls=%s in %s:%s  target=%s:%s (tgtParent=%s:%s) prop=%s",
                        cls, parCls, parName.constData(),
                        tgtCls, tgtName.constData(),
                        tgtParCls, tgtParName.constData(),
                        anim ? anim->propertyName().constData() : "?");
                }
            }
        }
        if (tname && DebugLog::enabled(cat)) {
            QWidget *w = qobject_cast<QWidget *>(watched);
            const char *cls = watched->metaObject()->className();
            const char *spont = event->spontaneous() ? "spont" : "synth";
            QByteArray objName = watched->objectName().toUtf8();
            QObject *par = watched->parent();
            const char *parCls = par ? par->metaObject()->className() : "null";
            QByteArray parName = par ? par->objectName().toUtf8() : QByteArray();
            QByteArray extra;
            if (std::string(cls) == "QPropertyAnimation") {
                auto *anim = qobject_cast<QPropertyAnimation *>(watched);
                if (anim) {
                    QObject *tgt = anim->targetObject();
                    const char *tgtCls = tgt ? tgt->metaObject()->className() : "null";
                    QByteArray tgtName = tgt ? tgt->objectName().toUtf8() : QByteArray();
                    extra = QByteArray(" target=") + tgtCls + ":" + tgtName
                          + " prop=" + anim->propertyName();
                }
            }
            if (w) {
                ANTS_LOG(cat, "%s [%s] cls=%s name=%s parent=%s:%s rect=%dx%d%s",
                    tname, spont, cls, objName.constData(),
                    parCls, parName.constData(),
                    w->width(), w->height(), extra.constData());
            } else {
                ANTS_LOG(cat, "%s [%s] cls=%s name=%s parent=%s:%s%s",
                    tname, spont, cls, objName.constData(),
                    parCls, parName.constData(), extra.constData());
            }
        }
    }
    // Dropdown-flicker kill-switch (app-level). See the construction-
    // site comment above `installEventFilter(this)` for rationale.
    // Extended 0.7.6 to also swallow HoverMove / HoverEnter /
    // HoverLeave in the same intra-action zone. Qt's style engine
    // consults WA_Hover tracking (a separate channel from
    // QMouseEvent) to update :hover pseudo-state on every cursor
    // position tick; without suppressing HoverMove here, each mouse
    // pixel over the active menubar item re-evaluated the stylesheet
    // for QMenuBar::item:hover → repainted the menubar → KWin
    // re-composited the translucent window → visible flicker on the
    // open dropdown that sits on top. User reported 2026-04-20 that
    // the 0.7.5 NoAnimStyle fix only partially addressed it (slight
    // reduction but still visible); this is the missing half.
    if (event->type() == QEvent::MouseMove
        || event->type() == QEvent::HoverMove
        || event->type() == QEvent::HoverEnter
        || event->type() == QEvent::HoverLeave) {
        QWidget *popup = QApplication::activePopupWidget();
        if (popup && popup->inherits("QMenu")) {
            QPoint gpos;
            if (auto *me = dynamic_cast<QMouseEvent *>(event)) {
                gpos = me->globalPosition().toPoint();
            } else if (auto *he = dynamic_cast<QHoverEvent *>(event)) {
                // QHoverEvent carries widget-local position; convert
                // via the hovered widget.
                if (auto *w = qobject_cast<QWidget *>(watched)) {
                    gpos = w->mapToGlobal(he->position().toPoint());
                } else {
                    gpos = QCursor::pos();  // fallback
                }
            } else {
                gpos = QCursor::pos();
            }
            QPoint barLocal = m_menuBar->mapFromGlobal(gpos);
            if (m_menuBar->rect().contains(barLocal)) {
                QAction *under  = m_menuBar->actionAt(barLocal);
                QAction *active = m_menuBar->activeAction();
                if (under && under == active) {
                    return true;  // intra-action motion, no-op
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
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
        if (m_claudeBgTasks) m_claudeBgTasks->setTranscriptPath(QString());
        if (m_claudeBgTasksBtn) m_claudeBgTasksBtn->hide();
        if (m_roadmapBtn) m_roadmapBtn->hide();
        m_roadmapPath.clear();
        if (m_repoVisibilityLabel) m_repoVisibilityLabel->hide();
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
    refreshBgTasksButton();
    refreshRoadmapButton();
    refreshRepoVisibility();
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
        // rc_protocol set-title pin wins over the format-driven label.
        // Same guard as the titleChanged signal handler.
        if (m_tabTitlePins.contains(w)) continue;
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

namespace {
// Translate Qt's QKeySequence string form ("Ctrl+Shift+F12", "F12",
// "Ctrl+Alt+`") to the freedesktop shortcut syntax accepted by the
// GlobalShortcuts portal ("CTRL+SHIFT+F12", "F12", "CTRL+ALT+grave").
// Modifier names uppercase (with Meta→LOGO); keys pass through with a
// handful of common punctuation → xkb-keysym translations. Unmapped
// keys pass through unchanged — at worst the portal rejects the
// preferred_trigger, in which case the binding still succeeds with no
// default and the user adjusts in System Settings. Kept deliberately
// minimal; full keysym coverage is xkbcommon's job, not ours.
QString qtKeySequenceToPortalTrigger(const QString &qtHotkey) {
    if (qtHotkey.isEmpty()) return {};
    const QStringList parts = qtHotkey.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(parts.size());
    for (const QString &raw : parts) {
        const QString upper = raw.toUpper();
        if (upper == QLatin1String("CTRL") ||
            upper == QLatin1String("ALT") ||
            upper == QLatin1String("SHIFT")) {
            out << upper;
        } else if (upper == QLatin1String("META") ||
                   upper == QLatin1String("WIN") ||
                   upper == QLatin1String("SUPER")) {
            out << QStringLiteral("LOGO");
        } else if (raw == QLatin1String("`")) {
            out << QStringLiteral("grave");
        } else if (raw == QLatin1String("'")) {
            out << QStringLiteral("apostrophe");
        } else if (raw == QLatin1String(" ")) {
            out << QStringLiteral("space");
        } else {
            // F-keys and letters pass through as-is. F1..F24, single
            // letters A..Z, and digits 0..9 are accepted verbatim by
            // every portal backend we've tested.
            out << raw;
        }
    }
    return out.join(QLatin1Char('+'));
}
}  // anonymous

void MainWindow::setupQuakeMode() {
    m_quakeMode = true;
    m_quakeVisible = true;

    // Platform-dispatch:
    //   X11:  Qt::WindowStaysOnTopHint + Qt::Tool + move() — standard
    //         _NET_WM_STATE_ABOVE path; the compositor honours client-side
    //         positioning.
    //   Wayland: the compositor owns the stacking order and positioning for
    //         regular toplevel surfaces — move() is ignored and there's no
    //         equivalent of _NET_WM_STATE_ABOVE. With LayerShellQt available
    //         at build time, we promote the window to a wlr-layer-shell-v1
    //         top-layer surface anchored to the top edge of the active
    //         screen. Without it, the Wayland path falls back to the Qt
    //         toplevel and lives with whatever the compositor decides.
    const bool isWayland = QGuiApplication::platformName().startsWith(
        QStringLiteral("wayland"), Qt::CaseInsensitive);

#ifdef ANTS_WAYLAND_LAYER_SHELL
    if (isWayland) {
        // Ensure the QWindow exists before configuring layer-shell properties,
        // which must be set BEFORE show() so the xdg_surface role upgrade to
        // zwlr_layer_surface_v1 happens at the right point in the Wayland
        // handshake. winId() on a QWidget forces a native window backing.
        create();
        if (QWindow *qw = windowHandle()) {
            auto *layer = LayerShellQt::Window::get(qw);
            layer->setLayer(LayerShellQt::Window::LayerTop);
            LayerShellQt::Window::Anchors anchors =
                LayerShellQt::Window::AnchorTop;
            anchors |= LayerShellQt::Window::AnchorLeft;
            anchors |= LayerShellQt::Window::AnchorRight;
            layer->setAnchors(anchors);
            layer->setExclusiveZone(0);  // don't push neighbours; we overlay
            layer->setKeyboardInteractivity(
                LayerShellQt::Window::KeyboardInteractivityOnDemand);
            layer->setScope(QStringLiteral("ants-terminal-quake"));
            layer->setCloseOnDismissed(false);
        }
    }
#endif

    if (!isWayland) {
        // X11 path — unchanged from pre-0.6.38 behaviour.
        setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint | Qt::Tool);
    }

    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        int h = geo.height() / 3;
        resize(geo.width(), h);
        if (!isWayland) {
            // On Wayland the compositor + layer-shell anchors do the
            // positioning; a move() there is silently ignored and muddies
            // the trace logs.
            move(geo.x(), geo.y());
        }
    }
    show();
}

void MainWindow::toggleQuakeVisibility() {
    if (!m_quakeMode) return;

    QScreen *screen = this->screen();
    if (!screen) return;
    QRect geo = screen->availableGeometry();
    int h = height();

    // On Wayland, client-side move() is silently ignored by the compositor
    // (true both with and without layer-shell — layer-shell anchors the
    // surface to a screen edge; without layer-shell the compositor picks
    // the position). The slide-up/down animation uses pos() as its Qt
    // property which is a no-op under Wayland, so the XCB-only animation
    // path degenerates to a plain show/hide toggle. Prefer the plain
    // toggle there rather than ship a broken animation that visibly snaps.
    const bool isWayland = QGuiApplication::platformName().startsWith(
        QStringLiteral("wayland"), Qt::CaseInsensitive);

    if (isWayland) {
        if (m_quakeVisible) {
            hide();
            m_quakeVisible = false;
        } else {
            show();
            raise();
            activateWindow();
            m_quakeVisible = true;
            if (auto *t = focusedTerminal()) t->setFocus();
        }
        return;
    }

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
            if (curIdx >= 0) {
                // Route through the pin map so the shell's next OSC 0/2
                // (Claude Code writes one every few seconds) and the 2 s
                // updateTabTitles tick don't stomp the manual name.
                // Empty string clears the pin and restores the
                // format-driven / shell-driven label — gives the user an
                // in-UI "un-rename" path, matching rc_protocol semantics.
                setTabTitleForRemote(curIdx, newName);
            }
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

void MainWindow::refreshBgTasksButton() {
    if (!m_claudeBgTasks || !m_claudeBgTasksBtn) return;
    // Resolve the transcript path scoped to the active tab's project
    // tree. activeSessionPath walks up the cwd, encodes each ancestor
    // to Claude Code's `<dashed-cwd>` form, and returns the newest
    // `.jsonl` from the deepest matching `~/.claude/projects/<…>/`
    // subdir. Without this scoping, sessions from *other* projects
    // (e.g. another tab's tree) would leak into the bg-tasks surface
    // — which is exactly the user-reported 2026-04-27 bug.
    QString cwd;
    if (auto *t = focusedTerminal()) cwd = t->shellCwd();
    QString path;
    if (m_claudeIntegration) path = m_claudeIntegration->activeSessionPath(cwd);
    m_claudeBgTasks->setTranscriptPath(path);

    const int running = m_claudeBgTasks->runningCount();
    const int total = m_claudeBgTasks->tasks().size();
    if (running <= 0) {
        // No active background work — keep the chrome quiet.
        m_claudeBgTasksBtn->hide();
        return;
    }
    m_claudeBgTasksBtn->setText(tr("Background Tasks (%1)").arg(running));
    m_claudeBgTasksBtn->setToolTip(
        tr("%1 running · %2 total in this session").arg(running).arg(total));
    m_claudeBgTasksBtn->show();
}

void MainWindow::showBgTasksDialog() {
    if (!m_claudeBgTasks) return;
    showStatusMessage(QStringLiteral("Background Tasks: opening…"), 1500);
    // Re-target the tracker before opening so the dialog reflects the
    // active tab's session, not whatever the tracker last saw.
    refreshBgTasksButton();
    auto *dlg = new ClaudeBgTasksDialog(m_claudeBgTasks, m_currentTheme, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::refreshRoadmapButton() {
    if (!m_roadmapBtn) return;
    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (!t) {
        m_roadmapBtn->hide();
        m_roadmapPath.clear();
        return;
    }
    const QString cwd = t->shellCwd();
    if (cwd.isEmpty()) {
        m_roadmapBtn->hide();
        m_roadmapPath.clear();
        return;
    }
    // Case-insensitive match — `ROADMAP.md` is the documented norm but
    // accept any case so projects that follow norms loosely (lowercase
    // `roadmap.md`, `Roadmap.md`) still get the button. Linux fs is
    // case-sensitive so a manual entryList scan is required.
    QString found;
    QDir dir(cwd);
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files)) {
        if (fi.fileName().compare(QStringLiteral("ROADMAP.md"),
                                  Qt::CaseInsensitive) == 0) {
            found = fi.absoluteFilePath();
            break;
        }
    }
    if (found.isEmpty()) {
        m_roadmapBtn->hide();
        m_roadmapPath.clear();
        return;
    }
    m_roadmapPath = found;
    m_roadmapBtn->show();
}

void MainWindow::showRoadmapDialog() {
    if (m_roadmapPath.isEmpty()) {
        // Defensive: refresh once in case the click came in on a stale
        // path. If still empty, nothing to show.
        refreshRoadmapButton();
        if (m_roadmapPath.isEmpty()) return;
    }
    showStatusMessage(QStringLiteral("Roadmap: opening…"), 1500);
    auto *dlg = new RoadmapDialog(m_roadmapPath, m_currentTheme, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

namespace {

// Walk up `start` looking for a `.git` entry (file or directory).
// Returns the absolute path to the directory containing `.git`, or
// empty if none found.
QString findGitRepoRoot(const QString &start) {
    if (start.isEmpty()) return {};
    QDir d(start);
    while (true) {
        if (QFileInfo::exists(d.filePath(QStringLiteral(".git"))))
            return d.absolutePath();
        if (!d.cdUp()) return {};
    }
}

// Parse `.git/config` for the `[remote "origin"] url = ...` line.
// Handles both `https://github.com/owner/repo[.git]` and
// `git@github.com:owner/repo[.git]` forms. Returns "owner/repo"
// (no `.git` suffix) for GitHub remotes; empty for non-GitHub or
// missing origin.
QString parseGithubOriginSlug(const QString &repoRoot) {
    QFile f(repoRoot + QStringLiteral("/.git/config"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QString section;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            section = line;
            continue;
        }
        if (section != QStringLiteral("[remote \"origin\"]")) continue;
        if (!line.startsWith(QStringLiteral("url"))) continue;
        const int eq = line.indexOf('=');
        if (eq < 0) continue;
        QString url = line.mid(eq + 1).trimmed();
        // strip a trailing .git so the slug compares cleanly.
        if (url.endsWith(QStringLiteral(".git"))) url.chop(4);
        // https://github.com/owner/repo
        const QString httpsHost = QStringLiteral("https://github.com/");
        const QString sshHost = QStringLiteral("git@github.com:");
        if (url.startsWith(httpsHost)) return url.mid(httpsHost.size());
        if (url.startsWith(sshHost)) return url.mid(sshHost.size());
        return {};  // origin exists but isn't GitHub
    }
    return {};
}

// Compare two SemVer-shape strings ("X.Y.Z"). Returns 1 if `a` > `b`,
// -1 if a < b, 0 if equal. Non-numeric components fall back to
// string compare so unexpected suffixes don't crash.
int compareSemver(const QString &a, const QString &b) {
    const QStringList ap = a.split('.');
    const QStringList bp = b.split('.');
    const int n = std::max(ap.size(), bp.size());
    for (int i = 0; i < n; ++i) {
        const QString as = i < ap.size() ? ap[i] : QStringLiteral("0");
        const QString bs = i < bp.size() ? bp[i] : QStringLiteral("0");
        bool aok = false, bok = false;
        const int ai = as.toInt(&aok);
        const int bi = bs.toInt(&bok);
        if (aok && bok) {
            if (ai != bi) return ai > bi ? 1 : -1;
        } else {
            const int c = QString::compare(as, bs);
            if (c != 0) return c > 0 ? 1 : -1;
        }
    }
    return 0;
}

}  // namespace

void MainWindow::refreshRepoVisibility() {
    if (!m_repoVisibilityLabel) return;

    // Probe `gh` once per session — caching the result avoids a
    // shell-out on every tab switch when the binary is missing.
    if (!m_ghAvailableProbed) {
        m_ghAvailable = !QStandardPaths::findExecutable(
            QStringLiteral("gh")).isEmpty();
        m_ghAvailableProbed = true;
    }
    if (!m_ghAvailable) { m_repoVisibilityLabel->hide(); return; }

    QString cwd;
    if (auto *t = focusedTerminal()) cwd = t->shellCwd();
    if (cwd.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    const QString repoRoot = findGitRepoRoot(cwd);
    if (repoRoot.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    const QString slug = parseGithubOriginSlug(repoRoot);
    if (slug.isEmpty()) { m_repoVisibilityLabel->hide(); return; }

    auto applyVisibility = [this](const QString &visibility,
                                  const QString &repoSlug) {
        if (!m_repoVisibilityLabel) return;
        if (visibility.isEmpty()) { m_repoVisibilityLabel->hide(); return; }
        const bool isPublic =
            visibility.compare(QStringLiteral("PUBLIC"),
                               Qt::CaseInsensitive) == 0;
        const QString label = isPublic ? tr("Public") : tr("Private");
        const Theme &th = Themes::byName(m_currentTheme);
        const QColor &col = isPublic ? th.ansi[2] : th.ansi[3];
        m_repoVisibilityLabel->setText(label);
        m_repoVisibilityLabel->setStyleSheet(
            QStringLiteral("QLabel { color: %1; padding: 0 6px; "
                           "font-weight: bold; }").arg(col.name()));
        m_repoVisibilityLabel->setToolTip(tr("%1 on GitHub").arg(repoSlug));
        m_repoVisibilityLabel->show();
    };

    // Cache hit (10 min TTL) → render immediately, skip the shell-out.
    constexpr qint64 kCacheTtlMs = 10 * 60 * 1000;
    const qint64 nowMs =
        QDateTime::currentDateTime().toMSecsSinceEpoch();
    auto it = m_repoVisibilityCache.find(repoRoot);
    if (it != m_repoVisibilityCache.end() &&
            (nowMs - it->fetchedAt) < kCacheTtlMs) {
        applyVisibility(it->visibility, slug);
        return;
    }

    // Miss → hide until the async query lands. Avoids flashing a
    // stale value from a different repo (the previous tab's).
    m_repoVisibilityLabel->hide();

    auto *proc = new QProcess(this);
    proc->setProgram(QStringLiteral("gh"));
    proc->setArguments({QStringLiteral("repo"), QStringLiteral("view"),
                        slug, QStringLiteral("--json"),
                        QStringLiteral("visibility"),
                        QStringLiteral("-q"),
                        QStringLiteral(".visibility")});
    QPointer<MainWindow> self(this);
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [self, proc, repoRoot, slug, applyVisibility](
                int exitCode, QProcess::ExitStatus status) {
                proc->deleteLater();
                if (!self) return;
                QString visibility;
                if (status == QProcess::NormalExit && exitCode == 0) {
                    visibility = QString::fromUtf8(
                        proc->readAllStandardOutput()).trimmed();
                }
                // Cache both hits and negative results — a 60 s
                // negative TTL avoids hammering on every tab switch
                // when `gh` is unauthenticated. The full TTL applies
                // to positive results; we encode "negative" by storing
                // an empty visibility with the same fetchedAt so the
                // hit-branch sees an empty string and hides.
                self->m_repoVisibilityCache[repoRoot] = {
                    visibility,
                    QDateTime::currentDateTime().toMSecsSinceEpoch()};
                applyVisibility(visibility, slug);
            });
    proc->start();
}

void MainWindow::checkForUpdates(bool userInitiated) {
    if (!m_updateAvailableLabel) return;
    if (!m_updateNam) m_updateNam = new QNetworkAccessManager(this);

    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/milnet01/ants-terminal/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "Ants-Terminal-Updater");

    QNetworkReply *reply = m_updateNam->get(req);
    QPointer<MainWindow> self(this);
    connect(reply, &QNetworkReply::finished, this,
            [self, reply, userInitiated]() {
        reply->deleteLater();
        MainWindow *win = self.data();
        if (!win) return;
        QLabel *label = win->m_updateAvailableLabel;
        if (!label) return;
        if (reply->error() != QNetworkReply::NoError) {
            if (userInitiated) {
                win->showStatusMessage(
                    win->tr("Update check failed: %1")
                        .arg(reply->errorString()),
                    5000);
            }
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return;
        QString tag = doc.object().value("tag_name").toString();
        if (tag.startsWith('v')) tag.remove(0, 1);
        if (tag.isEmpty()) return;
        win->m_latestRemoteVersion = tag;
        const QString current = QString::fromUtf8(ANTS_VERSION);
        if (compareSemver(tag, current) <= 0) {
            // Already up-to-date or running a newer dev build.
            label->hide();
            if (userInitiated) {
                win->showStatusMessage(
                    win->tr("Up to date — running v%1 (latest)")
                        .arg(current),
                    4000);
            }
            return;
        }
        const QString url = QStringLiteral(
            "https://github.com/milnet01/ants-terminal/releases/tag/v%1").arg(tag);
        label->setText(
            win->tr("<a href=\"%1\" style=\"color: #5DCFCF; text-decoration: none;\">"
                    "↗ Update v%2 available</a>").arg(url, tag));
        label->setToolTip(
            win->tr("Click to open release notes for v%1 in your browser. "
                    "Currently running v%2.").arg(tag, current));
        label->show();
    });
}

void MainWindow::handleUpdateClicked(const QString &url) {
    // Probe for either flavor of the AppImage updater. The GUI
    // (`AppImageUpdate`) is preferred when present — it shows a
    // progress window the user can dismiss; the CLI
    // (`appimageupdatetool`) is the fallback and runs silently.
    // QStandardPaths::findExecutable returns the absolute path or an
    // empty string — empty means the binary isn't on PATH.
    const QString gui = QStandardPaths::findExecutable(
        QStringLiteral("AppImageUpdate"));
    const QString cli = QStandardPaths::findExecutable(
        QStringLiteral("appimageupdatetool"));
    const QString updater = !gui.isEmpty() ? gui : cli;

    // `$APPIMAGE` is set by the AppImage runtime when the binary is
    // unpacked from an AppImage; it points at the on-disk AppImage
    // file. When unset, the user is running an unbundled build —
    // there's nothing to update in place, so fall back to the
    // browser flow.
    const QString appimagePath = qEnvironmentVariable("APPIMAGE");

    if (!updater.isEmpty() && !appimagePath.isEmpty()) {
        // 0.7.47 — confirm with the user before kicking the
        // in-place update. The updater itself doesn't auto-restart
        // the running binary; the user needs to quit + re-launch
        // to pick up the new version. Any active Claude Code
        // sessions in tabs will be killed by the relaunch and
        // need to be reconnected. Surface that explicitly so the
        // click isn't a footgun for users in the middle of an
        // agent run. User feedback 2026-04-27.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Update Ants Terminal"));
        box.setText(tr("Download and install the new version now?"));
        box.setInformativeText(tr(
            "AppImageUpdate will fetch the new release and write "
            "it alongside this binary in the background.\n\n"
            "To start using the new version you'll need to "
            "<b>quit and re-launch</b> Ants Terminal — any active "
            "Claude Code sessions in your tabs will be "
            "disconnected when you do, and will need to be "
            "reconnected after the restart."));
        box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        box.setDefaultButton(QMessageBox::Ok);
        if (auto *okBtn = box.button(QMessageBox::Ok))
            okBtn->setText(tr("Update"));
        if (box.exec() != QMessageBox::Ok) {
            showStatusMessage(tr("Update cancelled."), 3000);
            return;
        }

        // Detached spawn — the updater outlives this binary so the
        // user can quit and restart while the download runs. Qt 6
        // form: static `startDetached(program, args)`. Returns true
        // on successful fork; we surface the outcome via the status
        // bar message rather than a modal dialog.
        const bool ok = QProcess::startDetached(
            updater, QStringList{appimagePath});
        if (ok) {
            showStatusMessage(
                tr("AppImageUpdate launched — downloading the new "
                   "version. Quit and restart to use it."),
                8000);
            return;
        }
        // Fork failed — fall through to the browser fallback so the
        // user isn't left without recourse.
        showStatusMessage(
            tr("AppImageUpdate failed to launch — opening release "
               "page in browser instead."),
            5000);
    }

    // Fallback: open the release page in the user's default browser.
    // QDesktopServices::openUrl is the Qt 6 idiom; it dispatches to
    // xdg-open under XDG, the Win32 ShellExecute equivalent on
    // Windows, and `open` on macOS.
    QDesktopServices::openUrl(QUrl(url));
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
    auto *liveStatus = new QLabel(dialog);
    liveStatus->setObjectName(QStringLiteral("reviewLiveStatus"));
    liveStatus->setStyleSheet("color: gray; font-size: 11px;");
    auto *refreshBtn = new QPushButton("Refresh", dialog);
    refreshBtn->setObjectName(QStringLiteral("reviewRefreshBtn"));
    auto *closeBtn = new QPushButton("Close", dialog);
    auto *copyBtn = new QPushButton("Copy Diff", dialog);
    btnBox->addWidget(liveStatus);
    btnBox->addStretch();
    btnBox->addWidget(refreshBtn);
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

    // Shared state for the async probes. When all finish (or error
    // out), we rebuild the viewer HTML with the real data and
    // re-install the copy handler. std::shared_ptr because lambdas
    // outlive any single QProcess callback.
    //
    // 0.7.32 — branches + crossUnpushed added so the dialog surfaces
    // work that lives on branches OTHER than HEAD. Pre-fix, the only
    // commit-level signal was `@{u}..HEAD` which is HEAD-only — a
    // user with five feature branches each holding unpushed commits
    // would see "no unpushed" if they happened to be on a clean
    // branch. User feedback 2026-04-25.
    //
    // 0.7.32 (live updates) — every refresh constructs a fresh
    // ProbeState so an in-flight refresh whose probes outlive the
    // next refresh can't decrement the new pending counter and
    // render half-populated HTML. The current state is held by
    // QPointer-style ownership inside the runProbes lambda.
    struct ProbeState {
        QString cwd;
        QString status;
        QString diff;
        QString unpushed;
        QString branches;        // git for-each-ref refs/heads
        QString crossUnpushed;   // git log --branches --not --remotes
        int pending = 5;
    };

    QPointer<QDialog> dlgGuard(dialog);
    QPointer<QTextEdit> viewerGuard(viewer);
    QPointer<QPushButton> copyGuard(copyBtn);
    QPointer<QLabel> liveStatusGuard(liveStatus);
    const QString themeName = m_currentTheme;

    // 0.7.37 — last rendered HTML, shared across runProbes invocations.
    // Used by finalize() to (a) skip setHtml when the new render is
    // byte-identical to the last one (the common case during idle
    // live-update tics — preserves selection, cursor, AND scroll
    // byte-perfectly), and (b) detect "first render" on dialog open
    // (empty string sentinel — first render restores no scroll
    // position). User report 2026-04-25: "the constant resetting of
    // the text means that if I scroll, it resets to the beginning
    // every refresh."
    auto lastHtml = std::make_shared<QString>();

    // 0.7.32 (live updates) — runProbes spawns the five async git
    // probes and fires-and-forgets. Called once on dialog open, then
    // again every time the QFileSystemWatcher debounce timer fires
    // OR the user clicks Refresh. Each call constructs a fresh
    // ProbeState so concurrent in-flight probes from a previous
    // refresh can't poison the new render.
    auto runProbes = [this, cwd, dlgGuard, viewerGuard, copyGuard,
                      liveStatusGuard, themeName, lastHtml]() {
        if (!dlgGuard) return;
        if (liveStatusGuard) {
            liveStatusGuard->setText(QStringLiteral("● refreshing…"));
            liveStatusGuard->setStyleSheet(
                "color: #e0a020; font-size: 11px;");
        }

        auto state = std::make_shared<ProbeState>();
        state->cwd = cwd;

    // Finalizer: called once per probe. When pending hits 0, render
    // the full HTML.
    auto finalize = [state, dlgGuard, viewerGuard, copyGuard,
                     liveStatusGuard, themeName, lastHtml]() {
        if (--state->pending > 0) return;
        if (!dlgGuard || !viewerGuard) return;
        if (liveStatusGuard) {
            liveStatusGuard->setText(QStringLiteral(
                "● live — auto-refresh on git changes"));
            liveStatusGuard->setStyleSheet(
                "color: #4aa84a; font-size: 11px;");
        }

        // Lambda-local alias `lth` (lambda theme) — avoids shadowing the
        // outer `th` at the enclosing function scope.
        const Theme &lth = Themes::byName(themeName);
        QString html = QStringLiteral("<pre style='color: %1; background: %2;'>")
                           .arg(lth.textPrimary.name(), lth.bgPrimary.name());
        auto section = [&html, &lth](const QString &title) {
            html += QStringLiteral("<span style='color: %1; font-weight:600;'>"
                                  "━━ %2 ━━</span>\n")
                        .arg(lth.ansi[6].name(), title.toHtmlEscaped());
        };
        if (!state->status.isEmpty()) {
            section(QStringLiteral("Status"));
            for (const QString &line : state->status.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith("##"))
                    html += QStringLiteral("<span style='color: %1;'>")
                                .arg(lth.ansi[4].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
            html += "\n";
        }
        if (!state->unpushed.isEmpty()) {
            section(QStringLiteral("Unpushed commits (current branch)"));
            for (const QString &line : state->unpushed.split('\n'))
                html += QStringLiteral("<span style='color: %1;'>")
                            .arg(lth.ansi[3].name())
                     + line.toHtmlEscaped() + "</span>\n";
            html += "\n";
        }
        if (!state->branches.isEmpty()) {
            section(QStringLiteral("Branches"));
            // Each line: "<branch>\t<upstream>\t<track>\t<subject>".
            // <track> is "[ahead 2, behind 1]" / "[gone]" / empty.
            // Render as a simple monospaced summary so the user can
            // scan ahead/behind across every local branch.
            for (const QString &line : state->branches.split('\n')) {
                if (line.trimmed().isEmpty()) continue;
                const QStringList parts = line.split('\t');
                const QString branch   = parts.value(0);
                const QString upstream = parts.value(1);
                const QString track    = parts.value(2);
                const QString subject  = parts.value(3);
                QString trackHtml;
                if (track.contains("ahead") || track.contains("behind")) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>%2</span>")
                                    .arg(lth.ansi[3].name(), track.toHtmlEscaped());
                } else if (track.contains("gone")) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>%2</span>")
                                    .arg(lth.ansi[1].name(), track.toHtmlEscaped());
                } else if (upstream.isEmpty()) {
                    trackHtml = QStringLiteral(" <span style='color: %1;'>"
                                              "[no upstream]</span>")
                                    .arg(lth.ansi[1].name());
                }
                html += QStringLiteral("<span style='color: %1;'>%2</span>%3 "
                                      "<span style='color: %4;'>%5</span>\n")
                            .arg(lth.ansi[4].name(), branch.toHtmlEscaped(),
                                 trackHtml,
                                 lth.textSecondary.name(), subject.toHtmlEscaped());
            }
            html += "\n";
        }
        if (!state->crossUnpushed.isEmpty()) {
            section(QStringLiteral("Unpushed across all branches"));
            for (const QString &line : state->crossUnpushed.split('\n'))
                html += QStringLiteral("<span style='color: %1;'>")
                            .arg(lth.ansi[3].name())
                     + line.toHtmlEscaped() + "</span>\n";
            html += "\n";
        }
        if (!state->diff.isEmpty()) {
            section(QStringLiteral("Diff"));
            for (const QString &line : state->diff.split('\n')) {
                QString esc = line.toHtmlEscaped();
                if (line.startsWith('+') && !line.startsWith("+++"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[2].name()) + esc + "</span>\n";
                else if (line.startsWith('-') && !line.startsWith("---"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[1].name()) + esc + "</span>\n";
                else if (line.startsWith("@@"))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[4].name()) + esc + "</span>\n";
                else if (line.startsWith("diff ") || line.startsWith("index "))
                    html += QStringLiteral("<span style='color: %1;'>").arg(lth.ansi[3].name()) + esc + "</span>\n";
                else
                    html += esc + "\n";
            }
        }
        if (state->status.isEmpty() && state->diff.isEmpty() &&
            state->unpushed.isEmpty() && state->branches.isEmpty() &&
            state->crossUnpushed.isEmpty()) {
            html += QStringLiteral(
                "<span style='color: %1;'>No status, diff, or unpushed commits "
                "to report.</span>\n"
                "<span style='color: %2;'>If you expected changes here, check "
                "that `git status` works in this directory:\n  %3</span>\n")
                .arg(lth.ansi[3].name(), lth.textSecondary.name(),
                     state->cwd.toHtmlEscaped());
        }
        html += "</pre>";

        // 0.7.37 — preserve scroll position across live refreshes. The
        // 0.7.32 live-update path called setHtml unconditionally on every
        // probe completion (every git change, every 300ms debounce
        // window), which resets the QTextEdit document and snaps the
        // scroll bar back to the top. On a long diff with active live
        // updates the dialog became unscrollable — every flick of the
        // wheel races with the next refresh. User report 2026-04-25:
        // "the constant resetting of the text means that if I scroll, it
        // resets to the beginning every refresh."
        //
        // Two-layer fix:
        //   (1) Skip setHtml entirely when the rendered HTML is byte-
        //       identical to the last render (the common case — branch
        //       metadata refreshes that don't change anything visible).
        //       Preserves selection, cursor, AND scroll byte-perfectly.
        //   (2) When content does change, capture vertical/horizontal
        //       scroll positions before setHtml and restore them
        //       (clamped to the new max so a shorter render after a
        //       commit doesn't over-scroll). Selection is lost in this
        //       branch — Qt re-parses HTML into a fresh QTextDocument —
        //       but the dialog still feels stable.
        if (*lastHtml == html) {
            return;
        }
        const bool isFirstRender = lastHtml->isEmpty();
        *lastHtml = html;

        QScrollBar *vbar = viewerGuard->verticalScrollBar();
        QScrollBar *hbar = viewerGuard->horizontalScrollBar();
        const int vPos = (vbar && !isFirstRender) ? vbar->value() : 0;
        const int hPos = (hbar && !isFirstRender) ? hbar->value() : 0;

        viewerGuard->setHtml(html);

        if (vbar && !isFirstRender) vbar->setValue(std::min(vPos, vbar->maximum()));
        if (hbar && !isFirstRender) hbar->setValue(std::min(hPos, hbar->maximum()));

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
                    combined += "# Unpushed (current branch)\n" + state->unpushed + "\n\n";
                if (!state->branches.isEmpty())
                    combined += "# Branches\n" + state->branches + "\n\n";
                if (!state->crossUnpushed.isEmpty())
                    combined += "# Unpushed across all branches\n"
                              + state->crossUnpushed + "\n\n";
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

    // 0.7.32 — branch-aware sections. for-each-ref reports every
    // local branch with its upstream + ahead/behind. The
    // --branches --not --remotes log lists every commit reachable
    // from any local branch but NOT reachable from any remote-
    // tracking branch — i.e. unpushed work across every branch,
    // not just HEAD's lineage. Both probes are O(refs) and finish
    // in milliseconds even on large repos.
    runAsync({"for-each-ref", "refs/heads",
              "--format=%(refname:short)\t%(upstream:short)\t"
              "%(upstream:track)\t%(subject)"},
             &ProbeState::branches, state.get());
    runAsync({"log", "--branches", "--not", "--remotes",
              "--oneline", "--decorate"},
             &ProbeState::crossUnpushed, state.get());
    };  // close runProbes lambda

    // Initial probe spawn — populates the dialog right after show().
    runProbes();

    // 0.7.32 (live updates) — QFileSystemWatcher on the relevant
    // .git/* paths plus the working directory. Git operations
    // (commit, checkout, fetch, pull, branch -d, add, etc.) all
    // touch one or more of these paths; a debounce timer collapses
    // the burst into a single re-probe. Refresh button forces an
    // immediate re-probe regardless of the watcher.
    auto *watcher = new QFileSystemWatcher(dialog);
    auto *debounce = new QTimer(dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(300);  // 300 ms — fast enough to feel live,
                                 // slow enough to coalesce a `git pull`
                                 // burst (which fires fileChanged
                                 // O(refs) times in milliseconds).
    connect(debounce, &QTimer::timeout, dialog, [runProbes]() {
        runProbes();
    });

    auto addPathSafe = [watcher](const QString &path) {
        if (QFileInfo::exists(path)) watcher->addPath(path);
    };
    addPathSafe(cwd);
    const QString gitDir = cwd + QStringLiteral("/.git");
    addPathSafe(gitDir);
    addPathSafe(gitDir + QStringLiteral("/HEAD"));
    addPathSafe(gitDir + QStringLiteral("/index"));
    addPathSafe(gitDir + QStringLiteral("/refs/heads"));
    addPathSafe(gitDir + QStringLiteral("/refs/remotes"));
    addPathSafe(gitDir + QStringLiteral("/logs/HEAD"));

    // QFileSystemWatcher loses its watch on a file when the file is
    // atomically replaced via rename(2) — git uses this pattern for
    // HEAD/index/logs/HEAD updates. Re-add the path on each
    // fileChanged so subsequent updates also fire.
    auto onFsEvent = [watcher, debounce, addPathSafe](const QString &path) {
        if (QFileInfo::exists(path) && !watcher->files().contains(path) &&
            !watcher->directories().contains(path)) {
            addPathSafe(path);
        }
        debounce->start();
    };
    connect(watcher, &QFileSystemWatcher::fileChanged, dialog, onFsEvent);
    connect(watcher, &QFileSystemWatcher::directoryChanged, dialog, onFsEvent);

    // Manual Refresh — bypasses debounce so the user gets an
    // immediate response when they click. Useful when an external
    // tool (a build script, a different terminal) has changed state
    // outside the watched paths.
    connect(refreshBtn, &QPushButton::clicked, dialog, [runProbes]() {
        runProbes();
    });
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

    // The cached Settings dialog was constructed with `&m_config` and
    // populated its widgets from the then-current values. `m_config`'s
    // address is stable (value member), but its *contents* just got
    // replaced wholesale — the dialog's widget state is now stale, and
    // some tabs cache pre-edit values on sub-widgets that don't re-read
    // the Config pointer on every paint. Dropping the cached instance
    // forces a fresh construction on the next Preferences... open, which
    // re-reads every field from the live m_config. If the dialog is
    // currently visible, close it first so the user sees the transition
    // instead of a silent swap on next show.
    if (m_settingsDialog) {
        if (m_settingsDialog->isVisible()) m_settingsDialog->close();
        m_settingsDialog->deleteLater();
        m_settingsDialog = nullptr;
    }

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

    // Per-tab Claude glyph toggle lives in the paint-provider closure —
    // repaint so the toggle change takes effect on the next frame.
    // Also clear every tab tooltip so a stale "Claude: thinking…"
    // doesn't linger on a tab after the user turned the feature off.
    if (m_coloredTabBar) m_coloredTabBar->update();
    if (m_tabWidget && !m_config.claudeTabStatusIndicator()) {
        for (int i = 0; i < m_tabWidget->count(); ++i)
            m_tabWidget->setTabToolTip(i, QString());
    }

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
