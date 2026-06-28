#include "mainwindow.h"
#include "themes.h"             // ANTS-1325: include directly where Themes:: is called

#include "coloredtabbar.h"
#include "opaquemenubar.h"
#include "opaquestatusbar.h"
#include "terminalwidget.h"
#include "titlebar.h"
#include "commandpalette.h"
#include "dialogchrome.h"
#include "aidialog.h"
#include "sshdialog.h"
#include "settingsdialog.h"
#include "sessionmanager.h"
#include "remotecontrol.h"
#include "resolvedroot.h"      // ANTS-1401 — terminalForCaller helper
#include "reviewbuttonstate.h" // ANTS-1874 — Review-button porcelain predicate
#include "verifytrustmodal.h"  // ANTS-1337 Phase 2
#include "branchchip.h"           // ANTS-1109 helper
#include "clipboardguard.h"       // ANTS-1014 clipboard funnel
#include "dialogfocus.h"          // ANTS-1050 helper
#include "kwinpositiontracker.h"
#include "claudeallowlist.h"
#include "claudebgtasks.h"
#include "claudebgtasksdialog.h"
#include "claudetasklist.h"
#include "claudetasklistdialog.h"
#include "roadmapdialog.h"
#include "claudeintegration.h"
#include "claudestatuswidgets.h"
#include "claudetabtracker.h"
#include "mcpprojection.h"   // ANTS-2085 — mcp::setTerseDefault
#include "mcpspill.h"        // ANTS-2094 — mcp::setOffloadConfig / spillSweep
#include "mcporientation.h"  // ANTS-1897 — SessionStart hook installer.
#include "themedstylesheet.h"
#include "claudeprojects.h"
#include "claudetranscript.h"
#include "aboutdialogs.h"          // ANTS-1181 — About-Ants/About-Qt
#include "auditdialog.h"
#include "auditrunner.h"      // ANTS-1351 — server-side audit runner.
#include "testauditengine.h"  // ANTS-1397 — test_audit_* trio engine.
#include "coldeyesdialog.h"   // ANTS-1721 — native cold-eyes review dialog.
#include "testauditdialog.h"  // ANTS-1722 — native test-suite review dialog.
#include "indiereviewdialog.h"  // ANTS-1258 — native independent code review.
#include "shellutils.h"
#include "elidedlabel.h"
#include "globalshortcutsportal.h"
#include "debuglog.h"
#include "dialogshowtracer.h"
#include "diffviewer.h"           // ANTS-1145 carve-out

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
#include "luaengine.h"  // ANTS-2093 — project_query provider lambda
#endif

#include <algorithm>
#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
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
#include <QThread>  // ANTS-2103 — run audit_run on a worker thread (off-main-loop)
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
#include <QGuiApplication>
#include <QStyleHints>
#include <QInputDialog>
#include <QTableWidget>
#include <QHeaderView>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QPainter>
#include <QScopeGuard>
#include <QSystemTrayIcon>
#include <QWindow>

// POSIX kill(2) used by ANTS-1322 stale-MCP-socket sweep at startup.
// ANTS-1325: <signal.h> directly — clangd's unused-includes lint reads the
// standard literally and doesn't recognise that ::kill is reachable via
// <csignal> on glibc.
#include <signal.h>
#include <cerrno>

// ANTS-1323: configure-time build-date / build-time macros for the
// window-title build-badge.
#include "build_info.h"

#ifdef ANTS_WAYLAND_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

namespace {
// Sweep stale `/tmp/kwin_{pos,move,center}_ants_*.js` files. These are
// written by kwinpositiontracker and the mainwindow move/center helpers
// as `QTemporaryFile(autoRemove=false)` + chained-dbus removal on
// script-unload. A crash, SIGKILL, or dbus-send hang between write and
// unload orphans the file. No functional harm — KWin has already loaded
// its copy — but the files accumulate in /tmp. Sweep anything older
// than one hour on startup; that comfortably clears genuine orphans
// without racing an in-flight script that another instance just wrote.
// Runs once per process; a second MainWindow (File → New Window) does
// not re-sweep.
// Names of shells that don't warrant a confirm-on-close prompt.
// If a tab's only descendants are these, the close is silent.
const QSet<QString> &safeShellNames() {
    static const QSet<QString> kSet = {
        QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("fish"), QStringLiteral("sh"),
        QStringLiteral("ksh"),  QStringLiteral("dash"),
        QStringLiteral("ash"),  QStringLiteral("tcsh"),
        QStringLiteral("csh"),  QStringLiteral("mksh"),
        QStringLiteral("yash"),
    };
    return kSet;
}

// Return the comm of the first non-shell descendant of `shellPid`,
// or empty if every descendant is a safe shell (or there are no
// descendants). Walks the /proc/<pid>/task/<pid>/children tree
// transitively, capped at kMaxVisitedPids to avoid pathological
// cases. Linux-only (mirrors the existing claudeintegration probe).
QString firstNonShellDescendant(pid_t shellPid) {
    if (shellPid <= 0) return {};
    constexpr int kMaxVisitedPids = 256;
    QSet<pid_t> visited;
    QList<pid_t> queue;
    queue.append(shellPid);
    while (!queue.isEmpty() && visited.size() < kMaxVisitedPids) {
        const pid_t pid = queue.takeFirst();
        if (visited.contains(pid)) continue;
        visited.insert(pid);
        QFile childFile(QString("/proc/%1/task/%1/children").arg(pid));
        if (!childFile.open(QIODevice::ReadOnly)) continue;
        const QString children = QString::fromUtf8(childFile.readAll()).trimmed();
        childFile.close();
        for (const QString &cstr : children.split(' ', Qt::SkipEmptyParts)) {
            bool ok = false;
            const pid_t cpid = cstr.toInt(&ok);
            if (!ok || cpid <= 0) continue;
            QFile commFile(QString("/proc/%1/comm").arg(cpid));
            if (!commFile.open(QIODevice::ReadOnly)) continue;
            const QString comm = QString::fromUtf8(commFile.readAll()).trimmed();
            commFile.close();
            if (comm.isEmpty()) continue;
            if (!safeShellNames().contains(comm)) {
                return comm;
            }
            queue.append(cpid);
        }
    }
    return {};
}

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

// ANTS-1400 — stringify `ants::ResolvedRoot::Source` for the
// `caller_cwd_info` MCP verb envelope. PascalCase mirrors the
// enum identifiers so the JSON literal and the C++ symbol stay
// in lock-step. New enum values added in the future require a
// matching case here; -Wswitch catches the gap at compile time.
QString sourceToString(ants::ResolvedRoot::Source s) {
    using S = ants::ResolvedRoot::Source;
    switch (s) {
        case S::ExplicitMatch: return QStringLiteral("ExplicitMatch");
        case S::EmptyFallback: return QStringLiteral("EmptyFallback");
        case S::NoMatch:       return QStringLiteral("NoMatch");
        case S::Unresolvable:  return QStringLiteral("Unresolvable");
    }
    return QStringLiteral("Unresolvable");  // -Wreturn-type
}

}  // namespace

MainWindow::MainWindow(bool quakeMode, bool e2eMode, QWidget *parent)
    : QMainWindow(parent) {
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
    m_posTracker = new KWinPositionTracker(this);

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
    // ANTS-1323: route the initial title through onTitleChanged so
    // the version + build-date + build-time badge appears at startup,
    // not just after the first shell-title broadcast.
    onTitleChanged(QString());
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
                    // ANTS-1750 INV-10 — route through the queued dispatchTo()
                    // so the keybinding handler runs on the plugin's worker,
                    // not lua_* on the GUI thread (engineFor()->fireEvent()
                    // would block the UI on a slow handler).
                    m_pluginManager->dispatchTo(pluginName, PluginEvent::Keybinding,
                                                actionId);
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
    // ants.clipboard.write — capability-gated clipboard write.
    // ANTS-1014 — Lua plugins are untrusted (third-party code);
    // funnel through clipboardguard so the 1 MiB cap + NUL strip
    // apply uniformly with the OSC 52 path.
    connect(m_pluginManager, &PluginManager::clipboardWriteRequested, this,
            [](const QString &text) {
        clipboardguard::writeText(text,
            clipboardguard::Source::UntrustedPlugin);
    });
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

    // ANTS-1842 — register Config for DialogChrome D3 size persistence
    // (mirrors the setActiveTheme broadcast). Must precede any dialog
    // construction so resizable dialogs can restore their saved size.
    DialogChrome::setConfig(&m_config);

    // Apply saved theme
    applyTheme(m_config.theme());

    // Claude Code integration — must be set up BEFORE newTab(), otherwise
    // the null-guarded m_claudeIntegration->setShellPid() call in newTab()
    // is a no-op for the first tab and polling never starts (so the
    // Claude status widget never appears in the status bar).
    setupStatusBarChrome();

    // Create first tab (restoreSessions may replace it if there are saved sessions)
    newTab();

    // Restore saved sessions from previous run
    restoreSessions();

    // ANTS-1159 — periodic session save so a SIGSEGV / OOM-kill /
    // power loss can't discard everything since the last graceful
    // close. saveAllSessions() already short-circuits on
    // !sessionPersistence and on the 5 s uptime floor, so the
    // timer is a single Qt connect with no extra guards. Tab list
    // itself is also saved synchronously on tab create / close /
    // reorder (see saveTabOrderOnly) — this timer covers per-tab
    // scrollback / cwd / pinned-title.
    m_sessionSaveTimer = new QTimer(this);
    m_sessionSaveTimer->setInterval(30000);
    connect(m_sessionSaveTimer, &QTimer::timeout,
            this, &MainWindow::saveAllSessions);
    m_sessionSaveTimer->start();

    // ANTS-1159 — tab-order saved on every tab move. (Create /
    // close are hooked from inside newTab + performTabClose
    // since those paths run setup/teardown that should complete
    // before the save fires.)
    connect(m_coloredTabBar, &QTabBar::tabMoved,
            this, [this](int, int) { saveTabOrderOnly(); });

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
    // 0.7.54 (2026-04-27 indie-review) — accessible name for screen
    // readers. Powerline glyph in front of the branch name reads as
    // an unrecognised codepoint; the accessible name overrides that
    // with semantic text. Description updates dynamically via
    // updateStatusBar when the branch changes.
    m_statusGitBranch->setAccessibleName(tr("Git branch"));
    statusBar()->addWidget(m_statusGitBranch);

    // 0.7.49 — Repo visibility badge. Public/Private chip for the
    // active tab's GitHub repo. Was on the right (addPermanentWidget,
    // 0.7.45) but the user asked 2026-04-27 for it next to the branch
    // — repo provenance reads as "branch · visibility" naturally, and
    // the right side is busy with Claude Code chrome. Same sizePolicy
    // contract as the branch label: Fixed so it's never squeezed.
    // Hidden when the cwd isn't a GitHub-backed repo, when `gh` is
    // missing, or when authentication / network fails. Theme-coloured
    // and chip-styled in refreshRepoVisibility(); the foreground colour
    // (green ansi[2] for public, red ansi[3] for private) is preserved
    // from 0.7.45 — only the chip frame is new.
    m_repoVisibilityLabel = new QLabel(this);
    m_repoVisibilityLabel->setObjectName(QStringLiteral("repoVisibilityLabel"));
    m_repoVisibilityLabel->setSizePolicy(QSizePolicy::Fixed,
                                         QSizePolicy::Preferred);
    m_repoVisibilityLabel->setAccessibleName(tr("GitHub repository visibility"));
    m_repoVisibilityLabel->hide();
    statusBar()->addWidget(m_repoVisibilityLabel);

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
    m_statusMessage->setAccessibleName(tr("Status notification"));
    statusBar()->addWidget(m_statusMessage, 1);

    m_statusProcess = new QLabel(this);
    m_statusProcess->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_statusProcess->setAccessibleName(tr("Foreground process"));
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
    // ANTS-1219-INV-2: 2 s cadence is the upper bound on how long a
    // resolver-result swap can go un-propagated to the task-list
    // tracker. Any change here re-shapes the chip's freshness
    // contract — adjust the spec INV alongside.
    m_statusTimer->setInterval(2000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateTabTitles);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshReviewButton);
    // ANTS-1160 P2 (0.7.78) — RoadMap button + GitHub repo-type
    // badge both read shellCwd(), which depends on shellPid().
    // The first onTabChanged(0) fires from newTab()'s setCurrentIndex
    // BEFORE startShell() sets the PID, so shellCwd() returns empty
    // and the widget hides on the first tick. Wire to the 2-second
    // status timer so a stale-hidden state is recovered within 2 s
    // of any cwd change. Same fix shape as refreshBgTasksButton
    // (already correct via line 706 below). Spec: docs/specs/
    // ANTS-1160.md §9. Test: tests/features/roadmap_status_bar_refresh/.
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshRoadmapButton);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::refreshRepoVisibility);
    // 0.7.49 — also drive the background-tasks button refresh on the
    // status tick. Without this the liveness-sweep (mtime check on
    // /tmp/.../<id>.output) never re-runs while the transcript is
    // silent, leaving a phantom "Background Tasks (12)" chip when
    // every task has actually finished. User report 2026-04-27.
    connect(m_statusTimer, &QTimer::timeout, m_claudeStatusBarController, &ClaudeStatusBarController::refreshBgTasksButton);
    // ANTS-1158 — task-list chip ticks alongside bg-tasks. Same 2 s
    // cadence; cheap (one parseTranscript per fire on a 16 MiB-capped
    // file) but only meaningful when the focused tab has a Claude
    // session. The refresh function self-gates on transcript
    // presence.
    // ANTS-1219-INV-2: status-timer → refreshTasksButton connect.
    // Pairs with the 2 s setInterval above to bound resolver-swap
    // propagation latency.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshTasksButton);
    // ANTS-1226 — model recommender chip, same 2 s cadence.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshModelChip);
    // ANTS-1888 — passive per-tab model + thinking-level chip, same tick.
    // mtime short-circuit inside the method makes the per-tick cost zero
    // when the focused tab's transcript hasn't changed.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshModelStateChip);
    // ANTS-1735 §2.3 — autonomous switcher gate runs on the same tick.
    // Default-off via Config::claudeAutoModel().switch_enabled (INV-14);
    // the method short-circuits when disabled, so the cost is one
    // config read + early return.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshAutoModelSwitch);
    // ANTS-1951 — auto-confirm a user-typed /model "Switch model?" dialog on
    // the same tick, independently of the auto-switch master toggle. Gated by
    // claude.auto_model_confirm_user_switch (default on); cheap early return
    // when no dialog is visible.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            &ClaudeStatusBarController::maybeAutoConfirmUserModelSwitch);
    // ANTS-1735 §2.5 — outcome fill-in tick. Same 2 s timer, but the
    // method internally throttles to once per 30 s and bails fast when
    // the ledger is empty or has no pending records.
    // ANTS-1890 — qOverload<> disambiguates the no-arg production
    // overload from the path-injecting test overload added for INV-7.
    connect(m_statusTimer, &QTimer::timeout,
            m_claudeStatusBarController,
            qOverload<>(&ClaudeStatusBarController::fillPendingLedgerOutcomes));
    // ANTS-1735 §8 OQ-3 — first-run nudge. Controller fires this at most
    // once per process when Claude Code is running in the focused tab,
    // the switch is still default-off, and the persistent
    // claude.auto_model_nudge_shown flag is false. We show a one-shot
    // QMessageBox; either answer persists the flag so no future session
    // re-prompts.
    connect(m_claudeStatusBarController,
            &ClaudeStatusBarController::firstRunNudgeRequested,
            this, &MainWindow::showClaudeAutoModelNudge);
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
        // ANTS-1160 P2 — first refresh AFTER startShell() has had
        // a turn of the event loop and shellPid() is set. Without
        // this both widgets stay hidden until the user manually
        // switches tabs (v0.7.77 regression).
        refreshRoadmapButton();
        refreshRepoVisibility();
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
        wireQuakeHotkey();
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
    // ANTS-2049 — propagate the `--e2e` launch flag; this is the sole enabler
    // of the inject verbs (false on every normal launch and on secondary
    // File→New Window instances, which default e2eMode=false).
    m_remoteControl->setE2eMode(e2eMode);
    // ANTS-1337 Phase 2 — install the verify-changes trust client.
    // Chrome-layer VerifyTrustModalClient shows a QMessageBox when
    // verify_changes hits a .ants/verify.json whose SHA isn't
    // trusted; user can grant trust via "Trust this SHA" /
    // "Trust this repo". RemoteControl takes ownership.
    m_remoteControl->setVerifyTrustClient(
        std::make_unique<VerifyTrust::ModalClient>(this));
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
    // ANTS-2049 — `--e2e` forces the socket open past the default-false config
    // gate so a throwaway test instance is reachable without touching config.
    static const bool remoteControlGate = m_config.remoteControlEnabled();
    if (remoteControlGate || e2eMode) {
        m_remoteControl->start();
    }
}

MainWindow::~MainWindow() {
    // ANTS-1320 (review-button probe path): any in-flight QProcess
    // child (the `git status` review-changes probe, started by
    // refreshReviewChangesButton) emits finished/errorOccurred from
    // its destructor when Qt's deleteChildren forcibly terminates the
    // child. Those connected lambdas capture QPointer<MainWindow> and
    // dereference `.data()` — a downcast that is UB once the derived
    // (MainWindow) destructor body has returned (vptr swapped to
    // QWidget). UBSan-confirmed 2026-05-14. Disconnect this MainWindow
    // as receiver and kill the child cleanly here, BEFORE the implicit
    // destruction chain emits the racy signal.
    for (QProcess *p : findChildren<QProcess *>()) {
        p->disconnect(this);
        if (p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(500);
        }
    }
}

// ANTS-1181 — setupMenus() was historically a 947-line block stuffed
// inside this same TU. Each top-level menu is now its own helper so the
// menu-bar wiring can be located + read + edited independently of its
// neighbours. setupMenus() is the orchestrator; the helpers contain the
// per-menu body verbatim (no behaviour change).

namespace {
// ANTS-1982 — keep a menu open after toggling a NON-exclusive checkable item, so the
// user can flip several independent checkboxes (Session Logging, Visual
// Bell, Background Blur…) in one visit instead of the menu dismissing on
// the first click. Exclusive/radio group members (Themes, Opacity,
// Scrollback) keep Qt's default "pick one and close" — the
// actionGroup()->isExclusive() guard distinguishes a checkbox from a
// radio. No Q_OBJECT needed: only the virtual eventFilter() is used.
class StayOpenOnToggleFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::MouseButtonRelease) {
            if (auto *menu = qobject_cast<QMenu *>(obj)) {
                QAction *a = menu->activeAction();
                if (a && a->isEnabled() && a->isCheckable()
                    && !(a->actionGroup() && a->actionGroup()->isExclusive())) {
                    a->trigger();   // toggle checked state + fire triggered()
                    return true;    // swallow the release so the menu stays open
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
}  // namespace

void MainWindow::setupMenus() {
    setupFileMenu();
    setupEditMenu();
    setupViewMenu();
    setupSplitMenu();
    setupToolsMenu();
    setupSettingsMenu();
    setupHelpMenu();

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
            QString cmd = QString("claude \"Read %1 and fix any real issues found in the project audit."
                                  " Focus on bugs, security vulnerabilities, and code quality problems."
                                  " Ignore informational items like line counts and file sizes."
                                  " For each fix, explain what you changed and why.\"\n").arg(resultsFile);
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
        for (auto *t : terminals) t->setSessionLogging(checked);
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

                QList<TerminalWidget *> terminals = liveTerminals();
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
        AboutDialogs::showAboutAnts(this);
    });

    QAction *aboutQtAction = helpMenu->addAction("About &Qt...");
    connect(aboutQtAction, &QAction::triggered, this, [this]() {
        AboutDialogs::showAboutQt(this);
    });

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

QList<TerminalWidget *> MainWindow::liveTerminals() const {
    // ANTS-1182: O(N) over a small contiguous list rather than the
    // full QObject child tree. Returned snapshot is owned by the
    // caller so iteration is stable even if a terminal is destroyed
    // mid-loop. The pointers themselves remain owned by Qt's parent
    // chain.
    //
    // ANTS-1324: compact null entries (Qt auto-nulled them when the
    // wrapped TerminalWidget was destroyed) lazily here, rather than
    // eagerly via a destroyed() slot. The eager path fired UBSan
    // because the slot runs during ~QWidget() with vptr=QWidget, and
    // QPointer<TerminalWidget>::data() would static_cast that to
    // TerminalWidget*. m_allTerminals is `mutable` for this reason.
    m_allTerminals.removeIf(
        [](const QPointer<TerminalWidget> &p) { return p.isNull(); });
    QList<TerminalWidget *> live;
    live.reserve(m_allTerminals.size());
    for (const QPointer<TerminalWidget> &p : m_allTerminals) {
        live.append(p.data());
    }
    return live;
}

void MainWindow::connectTerminal(TerminalWidget *terminal) {
    // ANTS-1182: register with the flat all-terminals list so
    // iterate-all sites don't each walk the QObject child tree.
    // ANTS-1324: removal is lazy (see liveTerminals()) — no eager
    // destroyed() handler, because that handler would call
    // QPointer<TerminalWidget>::data() while ~QWidget() is on the
    // stack and trip UBSan -fsanitize=vptr.
    m_allTerminals.append(QPointer<TerminalWidget>(terminal));

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
        QList<TerminalWidget *> all = liveTerminals();
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
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setError(QString("Exit %1").arg(exitCode),
                                                  output.left(500),
                                                  10000);
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
            if (m_claudeStatusBarController)
                m_claudeStatusBarController->setPromptActive(false);
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
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(true);

        // Primary retraction: terminal scrollback scanner notices the
        // footer is gone. Now debounced against transient TUI repaints
        // (see terminalwidget.cpp:checkForClaudePermissionPrompt).
        // ANTS-1174: Qt::SingleShotConnection auto-disconnects on
        // first emission so we no longer need a heap-allocated
        // shared_ptr<Connection> just to capture-and-call-disconnect
        // from inside the lambda.
        connect(terminal, &TerminalWidget::claudePermissionCleared, addBtn,
                [addBtn, clearPromptActive, this]() {
            addBtn->deleteLater();
            clearStatusMessage();
            clearPromptActive();
        }, Qt::SingleShotConnection);

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
            // ANTS-1174: same SingleShotConnection treatment.
            connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                    addBtn, [addBtn, clearPromptActive, this](const QString &, bool) {
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            }, Qt::SingleShotConnection);
            connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                    addBtn, [addBtn, clearPromptActive, this](const QString &) {
                addBtn->deleteLater();
                clearStatusMessage();
                clearPromptActive();
            }, Qt::SingleShotConnection);
        }
    });

    // ANTS-1858 — AskUserQuestion / selection-prompt detection. Unlike
    // the permission path above there is no rule and no allow/deny
    // button: Claude is blocked on the user's choice, so we light the
    // owning tab's "awaiting input" dot + the "Claude: prompting" label
    // and nothing else. Routed by the emitting terminal pointer (no
    // session-id needed), mirroring the scroll-scan permission branch.
    connect(terminal, &TerminalWidget::claudeQuestionDetected, this,
            [this, terminal]() {
        if (terminal != focusedTerminal() && terminal != currentTerminal())
            return;
        const pid_t pid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (pid > 0)
            m_claudeTabTracker->markShellAwaitingInput(pid, true);
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(true);
    });
    connect(terminal, &TerminalWidget::claudeQuestionCleared, this,
            [this, terminal]() {
        const pid_t pid =
            (terminal && m_claudeTabTracker) ? terminal->shellPid() : pid_t(0);
        if (pid > 0)
            m_claudeTabTracker->markShellAwaitingInput(pid, false);
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->setPromptActive(false);
    });

    // ANTS-1858 follow-up — reliable hook-driven clear for the question
    // dot, mirroring the permission path's belt (see ~line 2302). The
    // footer-gone debounce in checkForClaudePermissionPrompt can't
    // complete while Claude streams output (the trailing-edge detect
    // timer rarely fires N=3 times), so the dot would stay orange after
    // the user answered. An AskUserQuestion is a mid-turn tool call:
    // PostToolUse (→ toolFinished) fires the instant it is answered and
    // Stop (→ sessionStopped) at end-of-turn — neither fires while the
    // question is still on screen, so this never clears prematurely.
    // clearClaudeQuestionPrompt no-ops unless a question is active and
    // resets the sticky flag so the next question re-lights.
    if (m_claudeIntegration) {
        connect(m_claudeIntegration, &ClaudeIntegration::toolFinished,
                terminal, [terminal](const QString &, bool) {
            terminal->clearClaudeQuestionPrompt();
        });
        connect(m_claudeIntegration, &ClaudeIntegration::sessionStopped,
                terminal, [terminal](const QString &) {
            terminal->clearClaudeQuestionPrompt();
        });
    }
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
    if (m_claudeStatusBarController && terminal->shellPid() > 0)
        m_claudeStatusBarController->trackBgShell(terminal->shellPid());

    // Hide tab bar when only one tab
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    // ANTS-1159 — persist the new tab in the on-disk order
    // immediately so a crash in the next 30 s window doesn't
    // lose it. saveTabOrderOnly's own guards short-circuit
    // when sessionPersistence is off or during the 5 s uptime
    // floor (the constructor's restoreSessions path lands here
    // for every restored tab and we don't want those replays
    // overwriting the on-disk order with a partially-built
    // list).
    saveTabOrderOnly();
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

// ANTS-1735 §8 OQ-3 — one-shot opt-in nudge. The controller decides
// when to fire it; this slot just shows the prompt, persists the
// shown-flag (regardless of answer), and flips the switch on Yes.
void MainWindow::showClaudeAutoModelNudge() {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Let Ants pick the Claude model?"));
    box.setText(tr("Ants can swap Claude Code between fast/cheap and "
                   "big/slow models for you automatically."));
    box.setInformativeText(
        tr("It only ever switches between turns and before you start "
           "typing, so it never interrupts. You can change this any time "
           "in Settings → General → \"Let Ants pick the Claude model "
           "for me\"."));
    auto *enableBtn = box.addButton(tr("Enable"), QMessageBox::AcceptRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(enableBtn);
    box.exec();

    Config cfg;
    cfg.setClaudeAutoModelNudgeShown(true);
    if (box.clickedButton() == enableBtn) {
        cfg.setClaudeAutoModelSwitch(true);
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

// ANTS-1911 — forward declaration for the file-static helper defined
// just below focusedTerminal(). Needed because the body of
// focusedTerminal() now calls activeTerminalInTab() (was added in the
// ANTS-1911 fix) but the static lives later in the translation unit.
static TerminalWidget *activeTerminalInTab(QWidget *root);

TerminalWidget *MainWindow::focusedTerminal() const {
    // ANTS-1911 — scope focus tracking to the CURRENTLY-SELECTED tab's
    // subtree. Pre-1911 the function walked QApplication::focusWidget()
    // first and only fell back to currentTerminal() when the walk
    // returned nullptr — but `QApplication::focusWidget()` is the
    // *global* focus across all windows, so a sibling tab whose
    // terminal still held keyboard focus (Qt does not always move
    // focus on a mouse-driven tab switch) could be returned even when
    // the user's *visually-current* tab is different. The status bar's
    // Claude chip + state label, the model chips, and a handful of
    // other callers ride this resolver — and a wrong-tab read leaks
    // the other tab's Claude state into the focused tab's chrome
    // (user report 2026-05-28 screenshots, ROADMAP ANTS-1911).
    //
    // The fix: get the current tab's root widget, then only accept a
    // QApplication::focusWidget() that lives inside that subtree.
    // Within-tab split-pane focus still resolves to the focused pane;
    // an unrelated tab's focus is rejected so the chrome stays
    // anchored to what the user sees.
    QWidget *currentTabRoot = m_tabWidget
        ? m_tabWidget->currentWidget() : nullptr;
    if (!currentTabRoot) return nullptr;
    QWidget *focused = QApplication::focusWidget();
    if (focused) {
        // ancestorOf accepts the widget itself as well.
        for (QWidget *w = focused; w; w = w->parentWidget()) {
            if (w == currentTabRoot) {
                // Focus is inside the current tab — walk up from
                // `focused` to find the enclosing TerminalWidget (so
                // a child line-edit or inner subwidget resolves to
                // its terminal).
                for (QWidget *p = focused; p; p = p->parentWidget()) {
                    if (auto *t = qobject_cast<TerminalWidget *>(p)) {
                        return t;
                    }
                }
                break;  // current-tab subtree but no terminal up the chain
            }
        }
    }
    // Fallback (no in-tab focus, or focus is in a foreign tab/window):
    // resolve to the visually-current tab's terminal so the chrome
    // tracks the user's view, not Qt's stale focus.
    return activeTerminalInTab(currentTabRoot);
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

    // Confirm-on-close (ANTS-1102): if the tab's shell has any non-shell
    // descendant running (vim, top, claude, tail -f, ...), ask before
    // tearing down. The dialog is async (Wayland-correct non-modal
    // pattern); confirmation calls performTabClose(idx) on Close-anyway.
    TerminalWidget *term = activeTerminalInTab(w);
    if (m_config.confirmCloseWithProcesses() && term && term->shellPid() > 0) {
        const QString descendant = firstNonShellDescendant(term->shellPid());
        if (!descendant.isEmpty()) {
            showCloseTabConfirmDialog(w, descendant);
            return;
        }
    }

    performTabClose(index);
}

void MainWindow::performTabClose(int index) {
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
    if (m_claudeStatusBarController && term && term->shellPid() > 0)
        m_claudeStatusBarController->untrackBgShell(term->shellPid());
    // ANTS-1131 — also prune the ClaudeIntegration plan-mode cache
    // for the same PID. Without this, m_planModeByPid grows
    // unbounded over a long session and Linux PID reuse can poison
    // a freshly-launched shell with a stale plan-mode flag from a
    // closed Claude tab.
    if (m_claudeIntegration && term && term->shellPid() > 0)
        m_claudeIntegration->forgetShell(term->shellPid());

    m_tabSessionIds.remove(w);
    m_tabTitlePins.remove(w);  // free pin alongside session id
    m_tabWidget->removeTab(index);
    w->deleteLater();

    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    if (auto *t = currentTerminal()) {
        t->setFocus();
    }

    // ANTS-1159 — persist the post-close order so a crash within
    // the next 30 s timer window doesn't resurrect the closed tab.
    saveTabOrderOnly();
}

void MainWindow::showCloseTabConfirmDialog(QWidget *tabWidget,
                                           const QString &processName) {
    // Wayland-correct non-modal QDialog pattern (mirrors the About
    // dialog at MainWindow ctor — see commit 6bea531 / 0.7.50 for the
    // QTBUG-79126 rationale). Plain QPushButtons; no setModal.
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Close tab?"));
    dlg->setObjectName(QStringLiteral("confirmCloseTabDialog"));
    auto *layout = new QVBoxLayout(dlg);

    auto *label = new QLabel(
        tr("This tab is running <b>%1</b>.<br>Close anyway? Long-running "
           "processes will be terminated.")
            .arg(processName.toHtmlEscaped()),
        dlg);
    label->setObjectName(QStringLiteral("confirmCloseTabBody"));
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(true);
    label->setAccessibleName(tr("Confirm tab close"));
    label->setAccessibleDescription(label->text());

    auto *dontAsk = new QCheckBox(
        tr("Don't ask again (close tabs silently in future)"), dlg);
    dontAsk->setObjectName(QStringLiteral("confirmCloseDontAsk"));

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancelBtn = new QPushButton(tr("Cancel"), dlg);
    cancelBtn->setObjectName(QStringLiteral("confirmCloseCancelBtn"));
    cancelBtn->setDefault(true);
    cancelBtn->setAutoDefault(true);
    auto *closeBtn = new QPushButton(tr("Close anyway"), dlg);
    closeBtn->setObjectName(QStringLiteral("confirmCloseProceedBtn"));
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(closeBtn);

    layout->addWidget(label);
    layout->addWidget(dontAsk);
    layout->addLayout(btnRow);

    // Track the actual widget; the index can shift if other tabs close
    // while this dialog is open.
    QPointer<QWidget> widgetRef(tabWidget);

    connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::close);
    connect(closeBtn, &QPushButton::clicked, this,
        [this, dlg, dontAsk, widgetRef]() {
            if (dontAsk->isChecked())
                m_config.setConfirmCloseWithProcesses(false);
            dlg->close();
            if (!widgetRef) return;
            const int idx = m_tabWidget->indexOf(widgetRef);
            if (idx >= 0) performTabClose(idx);
        });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
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

// ANTS-1392 — caller_cwd-anchored terminal lookup. Walks every tab
// (including split-pane subtrees via activeTerminalInTab) for a
// terminal whose canonical shellCwd matches the canonical callerCwd.
// First match wins.
//
// ANTS-1396 — contract split. Three cases:
//   1. caller_cwd is empty  → fall back to focusedTerminal()
//      (preserves the pre-ANTS-1392 contract for tools invoked
//      without the arg).
//   2. caller_cwd given, matches a tab → return that tab.
//   3. caller_cwd given, NO matching tab → return nullptr (do NOT
//      fall back to focused). A caller that names a specific cwd
//      is asking for *that* project's data; silently substituting
//      whatever happens to be focused leaks cross-project data
//      (the originating report was `get_git_status` returning the
//      Ants Terminal repo while the caller's cwd was a different
//      project with no Ants tab open).
//
// Callers already null-check the return (verified at the four
// terminalForCaller call sites in this file:
// `if (auto *t = terminalForCaller(...))` or `if (!t) return {};`).
TerminalWidget *MainWindow::terminalForCaller(const QString &callerCwd) const {
    // ANTS-1401 — single source of truth. The four-case decision tree
    // ANTS-1396 introduced now lives in `ants::resolveCallerCwdRoot`
    // (declared in resolvedroot.h, defined alongside the
    // `resolveRootCanonical` overloads in remotecontrol.cpp). This
    // function maps the tagged variant back to a TerminalWidget *.
    const ants::ResolvedRoot rr =
        ants::resolveCallerCwdRoot(this, callerCwd);
    switch (rr.source) {
        case ants::ResolvedRoot::Source::EmptyFallback:
            // Case 1 — legacy back-compat. Accessor may return nullptr
            // if no tab is focused; preserve that shape unchanged.
            return focusedTerminal();
        case ants::ResolvedRoot::Source::ExplicitMatch:
            // Case 2: caller_cwd canonicalises and matches an open tab.
            return rr.tabIndex ? terminalAtTab(*rr.tabIndex) : nullptr;
        case ants::ResolvedRoot::Source::NoMatch:
        case ants::ResolvedRoot::Source::Unresolvable:
            // Case 3: explicit caller_cwd, no match → nullptr.
            //         Unresolvable path also degrades to nullptr.
            return nullptr;
    }
    return nullptr;  // -Wreturn-type
}

int MainWindow::tabCount() const {
    return m_tabWidget->count();
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
    if (m_claudeStatusBarController && terminal->shellPid() > 0)
        m_claudeStatusBarController->trackBgShell(terminal->shellPid());

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

QJsonArray MainWindow::tabsAsJson() const {
    // Richer per-tab snapshot for the `tab-list` IPC verb (ANTS-1117).
    // Adds `shell_pid`, `claude_running`, and `color` on top of the
    // narrower `tabListForRemote` shape — keeps the existing `ls`
    // verb's contract unchanged for backward compat.
    QJsonArray tabs;
    const int n = m_tabWidget->count();
    for (int i = 0; i < n; ++i) {
        QJsonObject t;
        t["index"] = i;
        t["title"] = m_tabWidget->tabText(i);
        QString cwd;
        pid_t shellPid = 0;
        if (auto *term = activeTerminalInTab(m_tabWidget->widget(i))) {
            cwd = term->shellCwd();
            shellPid = term->shellPid();
        }
        t["cwd"] = cwd;
        t["shell_pid"] = qint64(shellPid);
        bool claudeRunning = false;
        if (m_claudeTabTracker && shellPid > 0) {
            // ANTS-1865 — surface the per-tab Claude glyph state so dot /
            // prompt-state behaviour is programmatically verifiable instead
            // of needing the user to eyeball the tab strip. `claude_state`
            // is the transcript-derived base state; `awaiting_input` /
            // `plan_mode` / `auditing` are the overlays that (together with
            // the base) determine the resolved dot, so a caller can compute
            // the expected glyph without the resolver.
            const auto ss = m_claudeTabTracker->shellState(shellPid);
            claudeRunning = (ss.state != ClaudeState::NotRunning);
            // Local snake_case mapping (no default → a new enum value trips
            // -Wswitch here, mirroring claudeStateName in claudestatuswidgets).
            QString stateName;
            switch (ss.state) {
                case ClaudeState::NotRunning: stateName = QStringLiteral("not_running"); break;
                case ClaudeState::Idle:       stateName = QStringLiteral("idle");        break;
                case ClaudeState::Thinking:   stateName = QStringLiteral("thinking");    break;
                case ClaudeState::ToolUse:    stateName = QStringLiteral("tool_use");    break;
                case ClaudeState::Compacting: stateName = QStringLiteral("compacting");  break;
            }
            if (stateName.isEmpty()) stateName = QStringLiteral("idle");
            t["claude_state"] = stateName;
            t["awaiting_input"] = ss.awaitingInput;
            // Lean envelope: emit the boolean/string overlays only when set.
            if (ss.planMode) t["plan_mode"] = true;
            if (ss.auditing) t["auditing"] = true;
            if (ss.state == ClaudeState::ToolUse && !ss.tool.isEmpty())
                t["tool"] = ss.tool;
        }
        t["claude_running"] = claudeRunning;
        QString color;
        if (m_coloredTabBar) {
            const QColor c = m_coloredTabBar->tabColor(i);
            if (c.isValid()) color = c.name();
        }
        t["color"] = color;
        tabs.append(t);
    }
    return tabs;
}

const QString &MainWindow::roadmapPathForRemote() const {
    return m_roadmapPath;
}

void MainWindow::applyTheme(const QString &name) {
    // ANTS-1138 — early-return when the requested theme matches
    // the current one. Pre-fix code always rewrote the entire
    // QSS even on no-op, which made the auto-profile-rules path
    // (updateStatusBar tick → checkAutoProfileRules → applyTheme
    // → setTheme → onConfigFileChanged → applyTheme) re-entrant
    // by accident. Idempotent setTheme + this guard close the
    // loop without depending on m_inConfigReload latency.
    if (name == m_currentTheme && !m_currentTheme.isEmpty())
        return;

    // ANTS-2097 — never run the app-wide restyle while a popup menu's
    // nested event loop is live. `qApp->setStyleSheet()` walks Qt's
    // global widget set and re-polishes every widget; the View→Themes
    // QMenu is still the active popup when its QAction::triggered fires
    // (this runs synchronously inside the menu's mouse-event handler,
    // see the crash backtrace frames QAction::activate ← sendMouseEvent),
    // and as that menu tears down it reaps a deleteLater'd transient
    // status-bar widget (the ANTS-1893 toast / Undo button, or a Claude
    // permission prompt) mid-walk — invalidating Qt's iterator and
    // leaving a garbage widget pointer the polish loop dereferences
    // (confirmed: SIGSEGV at `testb $1,0x30(%rax)` with rax=0x31, a
    // freed pointer). The ANTS-2024 DeferredDelete reap below is not
    // enough on its own because it's the menu's OWN nested loop, not a
    // pending DeferredDelete, that does the teardown. Deferring to the
    // next event-loop turn lets the menu fully close and the event stack
    // unwind, so the restyle runs against a quiescent widget set.
    // Startup / programmatic callers (no active popup) stay synchronous.
    if (QApplication::activePopupWidget()) {
        const QString deferred = name;
        QTimer::singleShot(0, this, [this, deferred]() { applyTheme(deferred); });
        return;
    }

    m_currentTheme = name;
    m_config.setTheme(name);
    // ANTS-1242 — broadcast the new theme to the DialogChrome
    // helper so any subsequently-opened dialog can theme its
    // custom frameless title bar without the call site needing
    // to plumb the name through.
    DialogChrome::setActiveTheme(name);

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

    // ANTS-1147 — QSS string-building moved into themedstylesheet::
    // helpers; the side-effecting setStyleSheet / setBackgroundFill /
    // re-polish calls stay here.
    //
    // ANTS-1128 (user reports 2026-04-30 of dropdown bg + Review Changes
    // dialog bg not matching the theme): apply the stylesheet at the
    // QApplication level, not the MainWindow level. Qt's stylesheet
    // engine only propagates through a widget's render subtree —
    // top-level QDialogs have their own paint chain and DO NOT inherit
    // the parent QWidget's stylesheet, even though they're QObject
    // children. qApp->setStyleSheet, by contrast, fans out to every
    // widget in the application including not-yet-constructed dialogs.
    // The selectors below (QMainWindow, QDialog, QMenu, etc.) are
    // scoped to specific widget types — they're still sourced from
    // the app-level stylesheet, just built by the helper now. The
    // previous setStyleSheet(this, ...) call was the comment's claim
    // to "Qt already propagates via the object tree", which is the
    // misconception the user's screenshots caught.
    //
    // ANTS-2024 — reap pending DeferredDelete events BEFORE the app-wide
    // restyle. setStyleSheet walks Qt's global widget collection; a
    // status-bar permission-prompt widget (claudestatuswidgets.cpp) that
    // was deleteLater()'d but not yet reaped can be torn down mid-walk
    // (e.g. by the theme QMenu's nested event loop), leaving a dangling
    // pointer in the set Qt iterates → SIGSEGV (d_ptr==NULL deref). Reaping
    // first makes the widget set quiescent so the snapshot Qt takes is
    // clean. NOTE: candidate fix — verify with a GUI repro (open a Claude
    // permission prompt, then change theme) before treating as closed.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    qApp->setStyleSheet(themedstylesheet::buildAppStylesheet(theme));

    // ANTS-1147 — invalidate the branch-chip cache. updateStatusBar's
    // tick will recompute and re-apply on the next call. Without
    // this, switching themes would leave the chip with the previous
    // theme's colours until the user changed branches (the QSS string
    // would still match the cache key).
    m_lastBranchChipValid = false;

    // Re-polish any already-open top-level dialog so live widgets pick
    // up the new palette without needing to re-instantiate. With qApp
    // as the stylesheet root, this is mostly redundant for new dialogs,
    // but cached singletons (m_settingsDialog, m_auditDialog …) need
    // the kick to refresh their already-styled state.
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
        m_menuBar->setStyleSheet(themedstylesheet::buildMenuBarStylesheet(theme));
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
        // Tab close (×) glyph: resting textSecondary, hover textPrimary,
        // hover background ansi-red (will-click cue). Drawn on a real
        // QToolButton per tab — Qt6 QSS can't render the data-URI SVG the
        // close-button rule used to carry (ANTS-2098).
        m_coloredTabBar->setCloseGlyphColors(theme.textSecondary,
                                             theme.textPrimary,
                                             theme.ansi[1]);
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

    // Status bar labels — ANTS-1147 routes through themedstylesheet
    // helpers (null-guarded for first call during construction).
    // Branch chip's foreground colour follows ANTS-1109 (0.7.62) —
    // green (theme.ansi[2], same role the visibility pill uses for
    // "Public") on main/master/trunk; amber (theme.ansi[3], same
    // role as "Private") on feature branches.
    if (m_statusGitBranch) {
        const bool primary =
            branchchip::isPrimaryBranch(m_gitCacheBranch);
        const QColor &col = primary ? theme.ansi[2] : theme.ansi[3];
        m_statusGitBranch->setStyleSheet(
            themedstylesheet::buildChipStylesheet(theme, col, /*leftMarginPx=*/4));
    }
    if (m_statusGitSep)
        m_statusGitSep->setStyleSheet(
            themedstylesheet::buildGitSeparatorStylesheet(theme));
    if (m_statusMessage)
        m_statusMessage->setStyleSheet(
            themedstylesheet::buildStatusMessageStylesheet(theme));
    if (m_statusProcess)
        m_statusProcess->setStyleSheet(
            themedstylesheet::buildStatusProcessStylesheet(theme));

    // Restyle Claude integration widgets
    if (m_claudeStatusBarController)
        m_claudeStatusBarController->applyTheme(m_currentTheme);

    // Apply colors + window opacity to ALL terminal widgets
    double opacity = m_config.opacity();
    QList<TerminalWidget *> terminals = liveTerminals();
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
    // ANTS-1142 — bail on non-KDE compositors before writing
    // any temp script. Pre-fix code unconditionally fired the
    // kwin-script + dbus-send chain on GNOME/Sway/Hyprland/etc.,
    // orphaning /tmp/kwin_move_ants_*.js files and triggering a
    // dbus-send that hits a non-existent service. Same guard as
    // KWinPositionTracker::setPosition (lifted to
    // kwinpositiontracker.h::kwinPresent for sharing).
    if (!kwinPresent()) return;
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
    // See kwinpositiontracker.cpp for rationale.
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

    // ANTS-1142 — bail on non-KDE compositors before writing any
    // temp script (same guard as moveViaKWin). The geometry
    // update above already moved the window via the
    // KWinPositionTracker abstraction; the kwin-script chain
    // below is the KDE-specific frameGeometry refresh.
    if (!kwinPresent()) return;

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
    QList<TerminalWidget *> terminals = liveTerminals();
    for (auto *t : terminals) {
        t->setFontSize(size);
    }
    showStatusMessage(QString("Font size: %1pt").arg(size), 3000);
}

void MainWindow::onTitleChanged(const QString &title) {
    // ANTS-1323: append a compact `version \u00b7 build-date build-time`
    // suffix so the running build is visible at a glance, without
    // opening Help \u2192 About. The full SHA + build type stay in the
    // About dialog. Format chosen for readability under the frameless
    // title bar's typical width.
    const QString badge = QStringLiteral("%1 \u00b7 %2 %3")
        .arg(QString::fromLatin1(ANTS_VERSION),
             QString::fromLatin1(ANTS_BUILD_DATE),
             QString::fromLatin1(ANTS_BUILD_TIME));
    QString windowTitle;
    if (title.isEmpty()) {
        windowTitle = QStringLiteral("Ants Terminal \u2014 ") + badge;
    } else {
        windowTitle = title + QStringLiteral(" \u2014 Ants Terminal \u00b7 ")
                            + badge;
    }
    setWindowTitle(windowTitle);
    m_titleBar->setTitle(windowTitle);
}


void MainWindow::collectActions(QMenu *menu, QObject *proxyParent,
                                QList<QAction *> &out) {
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
                            // Create a proxy action with prefixed name.
                            // Parent to proxyParent (transient holder)
                            // not `this` so previous-rebuild proxies
                            // get destroyed together. ANTS-1174.
                            auto *proxy = new QAction(prefix2 + sub2->text().remove('&'), proxyParent);
                            proxy->setShortcut(sub2->shortcut());
                            connect(proxy, &QAction::triggered, sub2, &QAction::trigger);
                            out.append(proxy);
                        }
                    }
                } else if (!sub->isSeparator() && !sub->text().isEmpty()) {
                    auto *proxy = new QAction(prefix + sub->text().remove('&'), proxyParent);
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

// ANTS-1159 — cheap tab-order-only save. Walks the tab widget,
// builds the tabOrder QStringList + active index, calls
// SessionManager::saveTabOrder. Does NOT touch the per-tab
// scrollback `.dat` files (saveAllSessions handles those, on
// the 30 s timer + closeEvent). Mirrors saveAllSessions's
// guards — sessionPersistence + 5 s uptime floor.
void MainWindow::saveTabOrderOnly() {
    if (!m_config.sessionPersistence()) return;
    if (m_uptimeTimer.elapsed() < 5000) return;

    QStringList tabOrder;
    int activeIndex = 0;
    const int currentIdx = m_tabWidget->currentIndex();

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        auto *t = activeTerminalInTab(w);
        if (!t) continue;

        QString tabId = m_tabSessionIds.value(w);
        if (tabId.isEmpty() && t != w)
            tabId = m_tabSessionIds.value(t);
        if (tabId.isEmpty()) continue;

        if (i == currentIdx)
            activeIndex = tabOrder.size();
        tabOrder.append(tabId);
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
            // ANTS-1375 — register the restored shell with the Claude
            // services. newTab + newTabForRemote already do this; the
            // restoreSessions path forgot, so per-tab Claude state dots
            // stayed dark on every tab carried across an Ants restart
            // (the bottom-bar status still works because tab-switch at
            // mainwindow.cpp:4340 wires ClaudeIntegration on focus,
            // but ClaudeTabTracker::m_shells is only ever populated by
            // trackShell — no other path reaches it).
            if (m_claudeTabTracker && tab.terminal->shellPid() > 0)
                m_claudeTabTracker->trackShell(tab.terminal->shellPid());
            if (m_claudeStatusBarController && tab.terminal->shellPid() > 0)
                m_claudeStatusBarController->trackBgShell(tab.terminal->shellPid());
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

// ANTS-1146 — formerly setupClaudeIntegration. Constructs
// ClaudeIntegration + ClaudeTabTracker (services owned by
// MainWindow), the ClaudeStatusBarController (chrome + per-session
// render state), wires the controller's signals to MainWindow's
// existing slots, then constructs the three orphan chrome items
// (Roadmap button, update-available QAction, 5 s startup
// update-check) that landed in this function for historical
// convenience and remain here as the status-bar chrome remainder.
// MCP-provider plumbing for ClaudeIntegration is split into
// setupClaudeMcpProviders below.
void MainWindow::setupStatusBarChrome() {
    m_claudeIntegration = new ClaudeIntegration(this);
    m_claudeTabTracker  = new ClaudeTabTracker(this);

    m_claudeStatusBarController =
        new ClaudeStatusBarController(statusBar(), this);
    m_claudeStatusBarController->setCurrentTerminalProvider(
        [this]{ return currentTerminal(); });
    m_claudeStatusBarController->setFocusedTerminalProvider(
        [this]{ return focusedTerminal(); });
    m_claudeStatusBarController->setTerminalAtTabProvider(
        [this](int i){ return terminalAtTab(i); });
    m_claudeStatusBarController->setTabIndicatorEnabledProvider(
        [this]{ return m_config.claudeTabStatusIndicator(); });
    m_claudeStatusBarController->attach(
        m_claudeIntegration, m_claudeTabTracker,
        m_coloredTabBar, m_tabWidget);
    m_claudeStatusBarController->applyTheme(m_currentTheme);

    connect(m_claudeStatusBarController, &ClaudeStatusBarController::reviewClicked,
            this, &MainWindow::showDiffViewer);
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::bgTasksClicked,
            this, &MainWindow::showBgTasksDialog);
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::tasksClicked,
            this, &MainWindow::showTaskListDialog);
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::allowlistRequested,
            this, &MainWindow::openClaudeAllowlistDialog);
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::reviewButtonShouldRefresh,
            this, &MainWindow::refreshReviewButton);
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::statusMessageRequested,
            this, [this](const QString &t, int ms){ showStatusMessage(t, ms); });
    connect(m_claudeStatusBarController, &ClaudeStatusBarController::statusMessageCleared,
            this, &MainWindow::clearStatusMessage);

    setupClaudeMcpProviders();

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

    // ANTS-1323: compact build-badge chip on the right edge of the
    // status bar — surfaces the running version + build date + build
    // time at-a-glance so the user can tell "am I on the latest?"
    // without opening Help → About. Full SHA + build type live in
    // the About dialog. Tooltip carries the long form for hover-detail.
    {
        auto *versionChip = new QLabel(this);
        versionChip->setSizePolicy(QSizePolicy::Fixed,
                                    QSizePolicy::Preferred);
        versionChip->setTextInteractionFlags(Qt::TextSelectableByMouse);
        versionChip->setText(QStringLiteral("v%1 · %2 %3")
            .arg(QString::fromLatin1(ANTS_VERSION),
                 QString::fromLatin1(ANTS_BUILD_DATE),
                 QString::fromLatin1(ANTS_BUILD_TIME)));
        versionChip->setToolTip(QStringLiteral(
            "Ants Terminal %1\nBuilt %2 %3 (%4)\ncommit %5")
            .arg(QString::fromLatin1(ANTS_VERSION),
                 QString::fromLatin1(ANTS_BUILD_DATE),
                 QString::fromLatin1(ANTS_BUILD_TIME),
                 QString::fromLatin1(ANTS_BUILD_TYPE),
                 QString::fromLatin1(ANTS_BUILD_COMMIT)));
        versionChip->setAccessibleName(tr("Ants Terminal build"));
        statusBar()->addPermanentWidget(versionChip);
    }

    // (0.7.45 repo visibility badge moved to the LEFT side next to the
    // git branch in 0.7.49 — see addWidget call earlier in the
    // constructor. Per-tab refresh via refreshRepoVisibility.)

    // 0.7.62 (ANTS-1124) — Update-available notifier as a top-level
    // menu-bar QAction. Promoted from a status-bar QLabel so the
    // one-shot "you have a new version" call-to-action reads as
    // visually loud chrome rather than competing with the steady-
    // state status widgets. Sits to the right of &Help by call
    // order; toggled via setVisible() rather than show()/hide() on
    // the underlying widget. URL is stashed on the action via
    // setData() so the triggered slot can replay it through
    // handleUpdateClicked().
    m_updateAvailableAction = new QAction(this);
    m_updateAvailableAction->setObjectName(
        QStringLiteral("updateAvailableAction"));
    m_updateAvailableAction->setVisible(false);
    connect(m_updateAvailableAction, &QAction::triggered, this, [this]() {
        const QString url =
            m_updateAvailableAction->data().toString();
        if (!url.isEmpty()) handleUpdateClicked(url);
    });
    m_menuBar->addAction(m_updateAvailableAction);

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
}

// ANTS-1833 — resolve+validate the caller_cwd that the inline audit_run /
// indie_review_dispatch handlers use as their in-flight-gate key. A
// non-existent root canonicalises to "" and would collapse every such
// call onto one shared key (one bogus caller blocks real sweeps); the
// dispatcher only enforces non-empty, not is-a-directory. On reject the
// `errOut` is a ready-to-return bad_cwd envelope. Returns true (and
// fills canonOut) only for an existing directory.
static bool resolveInflightCallerCwd(const QString &callerCwd,
                                     const char *tool,
                                     QString *canonOut, QString *errOut) {
    const QString canon = QFileInfo(callerCwd).canonicalFilePath();
    if (canon.isEmpty() || !QFileInfo(canon).isDir()) {
        QJsonObject env;
        env[QStringLiteral("ok")]    = false;
        // `bad_cwd` per docs/standards/mcp-error-codes.md — the precise
        // code for "caller_cwd does not exist or isn't a directory".
        env[QStringLiteral("code")]  = QStringLiteral("bad_cwd");
        env[QStringLiteral("error")] =
            QStringLiteral("%1: \"caller_cwd\" is not an existing directory")
                .arg(QLatin1String(tool));
        *errOut = QString::fromUtf8(
            QJsonDocument(env).toJson(QJsonDocument::Compact));
        return false;
    }
    *canonOut = canon;
    return true;
}

// ANTS-1146 — MCP-provider plumbing for ClaudeIntegration.
// Provides the scrollback / cwd / lastCommand / git-status /
// environment lookups MCP needs from MainWindow's tab/terminal
// state, then starts the hook server. Split out from
// setupClaudeIntegration because it isn't status-bar chrome.
void MainWindow::setupClaudeMcpProviders() {
    // ANTS-2085 — publish the terse-by-default preference to the MCP
    // dispatcher before any provider can serve. Default true (token-saving
    // on out of the box); the Settings Apply path and onConfigFileChanged
    // (external edits) re-publish it.
    mcp::setTerseDefault(m_config.claudeMcpTerseResponses());
    // ANTS-2094 — publish the result-offload config (default ON since the
    // 2026-06-25 fast-follow) and run a one-shot session-start sweep of
    // stale (>24 h) spill files.
    mcp::setOffloadConfig(m_config.claudeMcpOffloadLargeResults(),
                          m_config.claudeMcpOffloadThresholdBytes(),
                          m_config.claudeMcpOffloadHeadBytes());
    mcp::spillSweep();
    // ANTS-1322: reap stale MCP sockets from previously-crashed
    // ants-terminal instances. Without this they accumulate in
    // /tmp/ants-terminal-mcp-<PID> indefinitely; the mcp-bridge
    // picker would happily pick a stale one (newest by mtime) and
    // fail to connect, breaking the user-scoped MCP for every
    // non-Ants project. Sweep is cheap — one stat + one kill(0)
    // probe per file, dozens at most.
    {
        const QString tmp = QDir::tempPath();
        QDir tmpDir(tmp);
        const QStringList entries = tmpDir.entryList(
            QStringList{QStringLiteral("ants-terminal-mcp-*")},
            QDir::System | QDir::Files | QDir::Hidden);
        for (const QString &name : entries) {
            // Path-shape: ants-terminal-mcp-<PID>
            const QString pidStr = name.section(QChar('-'), -1);
            bool ok = false;
            const pid_t pid = pidStr.toLong(&ok);
            if (!ok) continue;  // doesn't look like our format
            if (pid == QApplication::applicationPid()) continue;
            // kill(pid, 0) returns 0 if the PID is live, -1 with
            // errno=ESRCH if it's gone. EPERM means someone else's
            // PID — leave that socket alone (not ours to clean).
            if (::kill(static_cast<pid_t>(pid), 0) == 0) continue;
            if (errno != ESRCH) continue;
            const QString full = tmp + QChar('/') + name;
            // S_ISSOCK guard prevents removing a regular file that
            // happens to share the name (defensive — unlikely).
            QFileInfo fi(full);
            if (fi.exists() && QFile(full).remove()) {
                qDebug() << "Reaped stale MCP socket:" << full;
            }
        }
    }

    // ANTS-1901 — master MCP gate. Seed the dispatcher's live bit, then
    // bind the socket + export ANTS_MCP_SOCKET only when enabled. When
    // off: no socket binds, no /tmp/ants-terminal-mcp-* file, no env
    // export (the orientation script self-silences on the missing var),
    // and any stale hook is removed below. Turning the switch ON takes
    // effect on the next launch (the socket binds here); turning it OFF
    // is honoured immediately by the dispatcher guard (ANTS-1901 § 2.4).
    const bool mcpOn = m_config.claudeMcpEnabled();
    m_claudeIntegration->setMcpEnabled(mcpOn);
    if (mcpOn) {
        QString mcpSocket = QDir::tempPath() + "/ants-terminal-mcp-" +
                            QString::number(QApplication::applicationPid());
        m_claudeIntegration->startMcpServer(mcpSocket);

        // ANTS-1897 INV-14 — export the MCP socket path into the parent
        // process env so every PTY spawned after this point (via the
        // non-flatpak `environ`-copy loop at ptyhandler.cpp:171)
        // inherits ANTS_MCP_SOCKET. The orientation prelude script
        // gates on this var being set + the socket file existing. The
        // ordering is correct because setupStatusBarChrome() (which
        // calls this) runs at L609 of the MainWindow ctor, BEFORE the
        // first newTab() at L612 spawns a PTY. Verified via grep.
        qputenv("ANTS_MCP_SOCKET", mcpSocket.toLocal8Bit());
    }

    // ANTS-1897 / ANTS-1901 — install the SessionStart hook only when the
    // master MCP gate AND the per-feature toggle are both on (default ON);
    // otherwise remove any stale Ants entry.
    if (mcpOn && m_config.claudeMcpOrientationEnabled()) {
        auto orient = ants::mcp_orientation::install();
        if (!orient.warning.isEmpty()) {
            qWarning().noquote() << "[mcp-orientation]" << orient.warning;
        }
        // INV-8 — first-run nudge latch. The visible signal to the
        // user is the prelude appearing at their next Claude session
        // start; latch the "nudge shown" flag so a future UI
        // enrichment (full QMessageBox or non-blocking toast) only
        // fires the once.
        if (orient.ok && !m_config.claudeMcpOrientationNudgeShown()) {
            m_config.setClaudeMcpOrientationNudgeShown(true);
        }
    } else {
        // User opted out — make sure no stale Ants entry remains.
        ants::mcp_orientation::uninstall();
    }
    // ANTS-1253: 12 tool handlers registered on the single-registry
    // ClaudeIntegration::registerToolProvider surface. Each handler
    // takes the JSON-RPC `arguments` object (extracts what it needs,
    // ignoring the rest) and returns the tool's response as a JSON
    // string. `get_session_info` is intentionally not registered —
    // it reads ClaudeIntegration's own state and is dispatched
    // inline (see claudeintegration.cpp processTools).
    // Indie-review-2026-05-14 lane-5 HI-3: every other envelope in the
    // codebase carries a `code` field so callers can dispatch on it
    // programmatically. This was the one outlier.
    // ANTS-1357: the literal lives at ClaudeIntegration::kMcpRcUnavailable
    // — shared so the idempotent-read cache can reject the same bytes
    // at insert time (INV-5(b)). Re-aliased here for local readability.
    static constexpr const char *kRcUnavailable =
        ClaudeIntegration::kMcpRcUnavailable;

    // ANTS-1782 — RC-delegate factory. Most MCP tools below are
    // byte-identical shims that differ only in which RemoteControl
    // cmd* verb they forward `args` to. This builds the handler so the
    // null-guard + serialise body lives in exactly one place rather
    // than being copy-pasted per tool. Non-shim tools (terminal-state
    // reads, in-flight gates, selective arg-forwarding, non-RC
    // delegates, the no-arg tab_list and multi-arg token_usage) keep
    // their inline lambdas — the factory only fits the
    // `cmd(args).toJson()` shape.
    auto rcDelegate =
        [this](QJsonDocument (RemoteControl::*fn)(const QJsonObject &))
            -> ClaudeIntegration::ToolHandler {
        return [this, fn](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            return QString::fromUtf8(
                (m_remoteControl->*fn)(args).toJson(QJsonDocument::Compact));
        };
    };

    // ANTS-2131 — off-main-thread RC-delegate factory. Same shim shape as
    // rcDelegate, but runs the cmd*() call on a short-lived worker thread and
    // joins it (QThread::wait() — a join, NOT an event pump) before returning.
    // Verbs that block on QProcess::waitForFinished (verify_changes, the
    // git/packaging-shelling debt_sweep_* verbs) otherwise freeze the GUI/MCP
    // thread for the duration of the child process. waitForFinished does not
    // pump the main loop's socket notifiers, so this is GUI-responsiveness —
    // not the use-after-free fix that worker-isolation buys the QEventLoop
    // verbs (audit_run/indie_review_dispatch, ANTS-2103/2104) — but it
    // structurally closes the "any MCP verb that blocks the main thread"
    // class. The cmd result (QJsonDocument) is a main-thread local captured by
    // reference; the QProcess + its buffers construct and live on the worker.
    auto rcDelegateWorker =
        [this](QJsonDocument (RemoteControl::*fn)(const QJsonObject &))
            -> ClaudeIntegration::ToolHandler {
        return [this, fn](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            QJsonDocument doc;
            QThread *worker = QThread::create(
                [this, fn, &args, &doc]() { doc = (m_remoteControl->*fn)(args); });
            worker->start();
            worker->wait();
            delete worker;
            return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        };
    };

    // ANTS-1301 — recent_errors. Scans the focused terminal's recent
    // scrollback for structured errors (compiler/lint/lua/test/python).
    // TabSpecific like get_text/get_scrollback; delegates to
    // RemoteControl::cmdRecentErrors. See docs/specs/ANTS-1301.md.
    m_claudeIntegration->registerToolProvider("recent_errors",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        rcDelegate(&RemoteControl::cmdRecentErrors));

    // ANTS-1312 — last_selection. Returns the focused (or routed) tab's
    // current selection text so Claude can pull the highlighted error /
    // trace / snippet without walking the scrollback to re-find it.
    // TabSpecific; delegates to RemoteControl::cmdLastSelection.
    m_claudeIntegration->registerToolProvider("last_selection",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        rcDelegate(&RemoteControl::cmdLastSelection));

    // ANTS-1636 — find_sources. Project-scoped topic-to-files
    // discovery; reads under <caller_cwd>/src + <caller_cwd>/tests.
    // Required contract — refuses without caller_cwd at the dispatcher.
    m_claudeIntegration->registerToolProvider("find_sources",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFindSources));

    m_claudeIntegration->registerToolProvider("get_scrollback",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            const int lines = args.value("lines").toInt(50);
            // ANTS-1392 — caller_cwd routes to the caller's tab when
            // present; falls back to focusedTerminal() otherwise.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            if (!t) return {};
            // ANTS-1500 — since_cursor incremental-fetch mode. Cursor
            // encodes the grid's monotonic scrollbackPushed counter at
            // the time of issue. On a follow-up call the server
            // computes the delta and only emits new content, or flips
            // cursor_stale:true when the gap exceeds ring capacity
            // (terminal restart, ring wrap). Absent since_cursor →
            // legacy raw-text response (current contract).
            const QString sinceStr =
                args.value(QStringLiteral("since_cursor")).toString();
            if (sinceStr.isEmpty()) {
                return t->recentOutput(lines);
            }
            const uint64_t currentPushed =
                t->grid()->scrollbackPushed();
            const int  ringCap = t->grid()->maxScrollback();
            const int  screenRows = t->grid()->rows();
            QJsonObject env;
            env[QStringLiteral("ok")]     = true;
            env[QStringLiteral("cursor")] =
                QString::number(currentPushed);
            bool parsedOk = false;
            const uint64_t since =
                sinceStr.toULongLong(&parsedOk);
            auto emitFullWindow = [&](const QString &reason) {
                env[QStringLiteral("cursor_stale")] = true;
                env[QStringLiteral("stale_reason")] = reason;
                env[QStringLiteral("content")] =
                    t->recentOutput(lines);
                return QString::fromUtf8(QJsonDocument(env)
                    .toJson(QJsonDocument::Compact));
            };
            if (!parsedOk) {
                return emitFullWindow(QStringLiteral("malformed_cursor"));
            }
            if (since > currentPushed) {
                // Counter went backwards — terminal restart or unrelated
                // session. Stale fallback returns the current window.
                return emitFullWindow(
                    QStringLiteral("counter_regressed"));
            }
            const uint64_t added = currentPushed - since;
            if (added > static_cast<uint64_t>(ringCap)) {
                // Lines lost beyond what the ring can replay.
                return emitFullWindow(QStringLiteral("ring_wrapped"));
            }
            // Up-to-date case: emit the delta lines + current screen so
            // the caller sees both newly-scrolled content and the live
            // viewport. content == "" only when nothing happened AND
            // the screen is empty.
            const int deltaPlusScreen =
                static_cast<int>(added) + screenRows;
            env[QStringLiteral("cursor_stale")] = false;
            env[QStringLiteral("content")] =
                t->recentOutput(deltaPlusScreen);
            return QString::fromUtf8(QJsonDocument(env)
                .toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_cwd",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1391: get_cwd's contract is "the terminal's cwd". The
            // caller passes caller_cwd so we resolve the right tab in a
            // multi-Ants-tab setup; empty caller_cwd → focused tab.
            // ANTS-1749 — route through terminalForCaller (like the
            // sibling tab-specific tools) so we only ever return an OPEN
            // tab's actual shellCwd(). Pre-fix this canonicalised and
            // echoed back ANY existing path the caller supplied — an
            // info-disclosure smell (confirms arbitrary path existence +
            // leaks the symlink-resolved form) that the ANTS-1295/-1392
            // tab-membership contract is meant to prevent.
            const QString callerCwd =
                args.value(QStringLiteral("caller_cwd")).toString();
            if (auto *t = terminalForCaller(callerCwd)) {
                const QString cwd = t->shellCwd();
                if (!cwd.isEmpty()) return cwd;
            }
            return QDir::currentPath();
        });
    m_claudeIntegration->registerToolProvider("get_last_command",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1392 — caller_cwd routes to the caller's tab.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            const int   exitCode = t ? t->lastExitCode()      : 0;
            const QString output = t ? t->lastCommandOutput() : QString();
            // ANTS-1503 — mode:"summary" envelope cuts the typical
            // 2-4 KiB body to a few hundred bytes when the caller
            // only needs exit + tail + duration. Default stays
            // "full" for back-compat with the original {exit_code,
            // output, failed} shape.
            QString mode = args.value("mode").toString().trimmed().toLower();
            if (mode.isEmpty()) mode = QStringLiteral("full");
            QJsonObject info;
            info["exit_code"] = exitCode;
            info["failed"]    = (exitCode != 0);
            if (mode == QStringLiteral("summary")) {
                const QStringList lines = output.split(QLatin1Char('\n'));
                info["line_count"] = lines.size();
                const int tailFrom = std::max<int>(0, lines.size() - 20);
                QJsonArray tail;
                for (int i = tailFrom; i < lines.size(); ++i) tail.append(lines.at(i));
                info["last_20"]    = tail;
                // Duration from the most recent completed OSC 133 region.
                qint64 ms = 0;
                if (t && t->grid()) {
                    const auto &regs = t->grid()->promptRegions();
                    for (auto it = regs.rbegin(); it != regs.rend(); ++it) {
                        if (it->commandEndMs > 0 && it->commandStartMs > 0) {
                            ms = it->commandEndMs - it->commandStartMs;
                            break;
                        }
                    }
                }
                info["ms"]   = static_cast<double>(ms);
                info["mode"] = QStringLiteral("summary");
            } else {
                info["output"] = output;
                info["mode"]   = QStringLiteral("full");
            }
            return QString::fromUtf8(
                QJsonDocument(info).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_git_status",
        ClaudeIntegration::CallerCwdContract::Required,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1392 — caller_cwd routes to the caller's tab.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            if (!t) return {};
            QString cwd = t->shellCwd();
            if (cwd.isEmpty()) return {};
            // ANTS-1748 — run the three probes concurrently on separate
            // QProcess objects instead of serially. Each waitForFinished
            // has a 2 s timeout; serial start/wait stalled the UI thread
            // up to ~6 s on a slow/locked repo (the exact freeze the
            // status-bar's async branch-probe exists to avoid). Started
            // together they run in parallel, so the worst-case GUI block
            // is one 2 s timeout, not three. Output format unchanged.
            QProcess branchProc, statusProc, logProc;
            for (QProcess *p : {&branchProc, &statusProc, &logProc})
                p->setWorkingDirectory(cwd);
            branchProc.start("git", {"rev-parse", "--abbrev-ref", "HEAD"});
            statusProc.start("git", {"status", "--porcelain", "-sb"});
            logProc.start("git", {"log", "--oneline", "-5"});
            for (QProcess *p : {&branchProc, &statusProc, &logProc})
                p->waitForFinished(2000);
            QStringList result;
            result << "Branch: " + QString::fromUtf8(branchProc.readAllStandardOutput()).trimmed();
            result << "Status:\n" + QString::fromUtf8(statusProc.readAllStandardOutput()).trimmed();
            result << "Recent commits:\n" + QString::fromUtf8(logProc.readAllStandardOutput()).trimmed();
            return result.join("\n\n");
        });
    m_claudeIntegration->registerToolProvider("get_environment",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            // ANTS-1392 — caller_cwd routes to the caller's tab.
            const QString callerCwd =
                args.value("caller_cwd").toString();
            auto *t = terminalForCaller(callerCwd);
            if (!t) return {};
            pid_t pid = t->shellPid();
            if (pid <= 0) return {};
            QFile envFile(QString("/proc/%1/environ").arg(pid));
            if (!envFile.open(QIODevice::ReadOnly)) return {};
            QByteArray raw = envFile.readAll();
            QStringList vars = QString::fromUtf8(raw).split('\0', Qt::SkipEmptyParts);
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

    // ANTS-1244 surface — the next 7 tools delegate to RemoteControl
    // cmd handlers so the IPC and MCP transports share verb logic.
    // ANTS-1247: roadmap_query threads the status filter through.
    m_claudeIntegration->registerToolProvider("roadmap_query",
        ClaudeIntegration::CallerCwdContract::Required,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            const QJsonValue statusVal = args.value("status");
            const QString status = statusVal.isString() ? statusVal.toString() : QString();
            // ANTS-1287 — optional `section` slug. isString() gate
            // matches the ANTS-1247 INV-9 pattern for `status`.
            const QJsonValue sectionVal = args.value("section");
            const QString section = sectionVal.isString() ? sectionVal.toString() : QString();
            // ANTS-1856 — optional `id` single-item selector. Same
            // isString() gate as status/section; empty/missing → the
            // handler takes its existing list path.
            const QJsonValue idVal = args.value("id");
            const QString idArg = idVal.isString() ? idVal.toString() : QString();
            // ANTS-1726 — optional `ids` plural-selector. Forwarded
            // VERBATIM as a JSON array (not type-coerced here) so the
            // handler can emit bad_args on malformed input. Same
            // silent-drop hazard ANTS-1856 fixed for the singular id.
            const QJsonValue idsVal = args.value("ids");
            const QJsonArray idsArg = idsVal.isArray() ? idsVal.toArray() : QJsonArray();
            // ANTS-1393 — forward caller_cwd so cmdRoadmapQuery's
            // per-project ROADMAP.md resolution at
            // remotecontrol.cpp:738 sees it. Without this the selective
            // rebuild silently dropped the field and the ANTS-1391
            // fix had no effect on roadmap_query.
            const QJsonValue cwdVal = args.value("caller_cwd");
            const QString callerCwd = cwdVal.isString() ? cwdVal.toString() : QString();
            QJsonObject req;
            if (!status.isEmpty()) req["status"] = status;
            if (!section.isEmpty()) req["section"] = section;
            if (!idArg.isEmpty()) req["id"] = idArg;
            if (!idsArg.isEmpty()) req["ids"] = idsArg;
            if (!callerCwd.isEmpty()) req["caller_cwd"] = callerCwd;
            // ANTS-1437 — forward `mode` so section_index dispatch
            // sees it. Same isString() gate as status/section. Empty
            // / missing → cmdRoadmapQuery defaults to "bullets".
            const QJsonValue modeVal = args.value("mode");
            if (modeVal.isString()) {
                const QString mode = modeVal.toString();
                if (!mode.isEmpty()) req["mode"] = mode;
            }
            // ANTS-1398 forward-fix — `include_section_headers` was
            // also dropped here. Caught while landing ANTS-1437.
            const QJsonValue inclVal = args.value("include_section_headers");
            if (inclVal.isBool()) req["include_section_headers"] = inclVal.toBool();
            // ANTS-1425 — forward `include_narrator_bullets` opt-in.
            const QJsonValue inclNarVal = args.value("include_narrator_bullets");
            if (inclNarVal.isBool()) req["include_narrator_bullets"] = inclNarVal.toBool();
            // ANTS-1586 — forward `include_body` opt-in. Was silently
            // dropped here, so `cmdRoadmapQuery` always read the
            // default-false branch and stripped the body field at
            // emission. Same isBool() gate as the other include_*
            // forwards.
            const QJsonValue inclBodyVal = args.value("include_body");
            if (inclBodyVal.isBool()) req["include_body"] = inclBodyVal.toBool();
            // ANTS-1436 — forward offset/limit VERBATIM (not
            // type-gated) so the handler can emit bad_args on
            // non-numeric. The only roadmap_query dispatch lambda
            // that breaks the silent-drop pattern; deliberate per
            // INV-8.
            if (args.contains("offset")) req["offset"] = args.value("offset");
            if (args.contains("limit"))  req["limit"]  = args.value("limit");
            return QString::fromUtf8(
                m_remoteControl->cmdRoadmapQuery(req).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("tab_list",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [this](const QJsonObject &) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            return QString::fromUtf8(
                m_remoteControl->cmdTabList().toJson(QJsonDocument::Compact));
        });
    // ANTS-1424 — roadmap_log: append a new bullet to ROADMAP.md.
    // Required-contract gated at the dispatcher (ANTS-1404), so
    // absent caller_cwd refuses upstream before this lambda runs.
    m_claudeIntegration->registerToolProvider("roadmap_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdRoadmapLog));
    // ANTS-1548 — changelog_log: token-frugal Keep-a-Changelog writer.
    // Write op → Required contract (refuses absent caller_cwd upstream).
    m_claudeIntegration->registerToolProvider("changelog_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdChangelogLog));
    // ANTS-1583 — roadmap_branch_drift: compare ROADMAP ✅ entries'
    // cited commit SHAs against HEAD's reachable history. caller_cwd
    // is Required (ANTS-1404 contract registered below).
    m_claudeIntegration->registerToolProvider("roadmap_branch_drift",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdRoadmapBranchDrift));
    // ANTS-1351 — audit_run server-side runner. Inline in-flight gate
    // via ClaudeIntegration::verbInFlight* (§ 2.4 of v4 spec) — no
    // class abstraction; two consumers (this verb + ANTS-1397's
    // test_audit_partition) don't justify the helper class.
    // caller_cwd is Required (ANTS-1404 contract registered in
    // callerCwdContractFor); dispatcher refuses upstream when absent.
    m_claudeIntegration->registerToolProvider("audit_run",
        ClaudeIntegration::CallerCwdContract::Required,
        [this](const QJsonObject &args) -> QString {
            const QString callerCwd = args.value(
                QStringLiteral("caller_cwd")).toString();
            // ANTS-1833 — reject a non-existent root before it can
            // collapse the in-flight key (canon would be empty).
            QString canon, badEnv;
            if (!resolveInflightCallerCwd(callerCwd, "audit_run",
                                          &canon, &badEnv))
                return badEnv;
            // In-flight gate (INV-11).
            const qint64 existing =
                m_claudeIntegration->verbInFlightTryAcquire(
                    QStringLiteral("audit_run"), canon);
            if (existing >= 0) {
                QJsonObject env;
                env["ok"]               = false;
                env["code"]             = QStringLiteral("already_running");
                env["error"]            = QStringLiteral(
                    "audit_run: a sweep is already in flight for this "
                    "project root; retry after it completes");
                env["running_since_ms"] =
                    QDateTime::currentMSecsSinceEpoch() - existing;
                env["retry_after_ms"]   = 5000;  // INV-9 hint
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            // RAII: release the in-flight slot on EVERY exit path (including
            // an exception or a future early return), honouring the header's
            // documented guard contract. indie-review-2026-05-21.
            const auto inFlightGuard = qScopeGuard([this, &canon] {
                m_claudeIntegration->verbInFlightRelease(
                    QStringLiteral("audit_run"), canon);
            });
            // Build the engine request.
            AuditRunner::RunRequest req;
            req.projectRoot = callerCwd;
            const QJsonArray toolsArr = args.value(
                QStringLiteral("tools")).toArray();
            for (const QJsonValue &v : toolsArr)
                req.tools.append(v.toString());
            req.scope = args.value(QStringLiteral("scope")).toString();
            if (args.value(QStringLiteral("cap_per_tool_seconds"))
                    .isDouble()) {
                req.capPerToolSeconds = args.value(
                    QStringLiteral("cap_per_tool_seconds")).toInt();
            }
            req.suppressionsMode =
                args.value(QStringLiteral("suppressions")).toString();
            const QJsonArray formatsArr = args.value(
                QStringLiteral("formats")).toArray();
            for (const QJsonValue &v : formatsArr)
                req.formats.append(v.toString());
            if (args.value(QStringLiteral("top_findings_count"))
                    .isDouble()) {
                req.topFindingsCount = args.value(
                    QStringLiteral("top_findings_count")).toInt();
            }
            // ANTS-1512 — scoped-check mode: narrow the tool's scope
            // to specific paths and/or a specific check set.
            const QJsonArray pathsArr = args.value(
                QStringLiteral("paths")).toArray();
            for (const QJsonValue &v : pathsArr)
                req.paths.append(v.toString());
            const QJsonArray checksArr = args.value(
                QStringLiteral("checks")).toArray();
            for (const QJsonValue &v : checksArr)
                req.checks.append(v.toString());
            // ANTS-2103 — run the audit on a worker thread so its internal
            // QEventLoop (auditrunner.cpp), which multiplexes the per-tool
            // QProcesses, lives OFF the main thread. Running it synchronously
            // here spun that nested QEventLoop on the GUI/MCP thread, which
            // reentrantly delivered QLocalSocket read-notifications and freed
            // the live MCP socket mid-dispatch -> use-after-free SIGSEGV (the
            // ANTS-2101 write-path guard was necessary but not sufficient; the
            // deeper hazard is pumping the main event loop at all). This
            // realises the INV-9 worker-thread isolation auditrunner.h already
            // documents. QThread::wait() blocks this thread via a join — it
            // does NOT pump events — so no foreign socket notification fires
            // during the sweep. (The GUI still freezes for the sweep duration;
            // a fully async dispatch is the larger INV-9 follow-up.)
            AuditRunner::RunResult r;
            {
                QThread *worker = QThread::create(
                    [&req, &r]() { r = AuditRunner::runAudit(req); });
                worker->start();
                worker->wait();
                delete worker;
            }
            // (in-flight slot released by inFlightGuard on scope exit)
            // Serialise envelope.
            QJsonObject env;
            if (!r.ok) {
                env["ok"]    = false;
                env["code"]  = r.code;
                env["error"] = r.error;
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            env["ok"] = true;
            QJsonObject byTool;
            for (auto it = r.byTool.constBegin();
                 it != r.byTool.constEnd(); ++it) {
                QJsonObject t;
                t["status"]              = it->status;
                t["elapsed_ms"]          = it->elapsedMs;
                t["raw_count"]           = it->rawCount;
                t["after_filter_count"]  = it->afterFilterCount;
                t["samples"]             = it->samples;
                byTool[it.key()]         = t;
            }
            env["by_tool"]          = byTool;
            env["total_raw"]        = r.totalRaw;
            env["total_actionable"] = r.totalActionable;
            env["noise_rate_pct"]   = r.noiseRatePct;
            // ANTS-2032 — explicit partiality signal: true when a tool
            // timed out / crashed but the rest of the run still produced
            // results (and the SARIF artifact below). `incomplete_tools`
            // lists the offenders so the caller need not scan by_tool[].
            env["partial"]          = r.partial;
            if (!r.incompleteTools.isEmpty()) {
                QJsonArray inc;
                for (const QString &t : r.incompleteTools) inc.append(t);
                env["incomplete_tools"] = inc;
            }
            if (!r.sarifPath.isEmpty())
                env["sarif_path"] = r.sarifPath;
            if (!r.htmlPath.isEmpty())
                env["html_path"] = r.htmlPath;
            QJsonArray skipped;
            for (const auto &ts : r.toolsSkipped) {
                QJsonObject s;
                s["tool"]   = ts.tool;
                s["reason"] = ts.reason;
                skipped.append(s);
            }
            env["tools_skipped"]    = skipped;
            env["elapsed_total_ms"] = r.elapsedTotalMs;
            env["samples_truncated"]= r.samplesTruncated;
            if (!r.topFindings.isEmpty())
                env["top_findings"] = r.topFindings;
            // ANTS-1555 — per-project `.audit_cache/` surface.
            // `cache_path` is set only when the SARIF landed in
            // `<root>/.audit_cache/`; `prior_run` carries the
            // pre-existing manifest's last_run snapshot (empty
            // object on a project's first sweep). ANTS-1504 reads
            // `prior_run.commit` as the since-last-run diff anchor
            // (precise findings delta deferred — ANTS-1504 § 5).
            if (!r.cachePath.isEmpty())
                env["cache_path"] = r.cachePath;
            if (!r.priorRun.isEmpty())
                env["prior_run"] = r.priorRun;
            // ANTS-1504 — narrowing-scope surface.
            if (!r.scopeResolved.isEmpty())
                env["scope_resolved"] = r.scopeResolved;
            if (!r.scopeAnchorCommit.isEmpty())
                env["scope_anchor_commit"] = r.scopeAnchorCommit;
            if (!r.scopeResolved.isEmpty())
                env["changed_files_count"] = r.changedFilesCount;
            if (!r.scopeDemoted.isEmpty()) {
                env["scope_demoted"] = r.scopeDemoted;
                env["scope_demoted_reason"] = r.scopeDemotedReason;
            }
            if (r.noChanges)
                env["no_changes"] = true;
            // ANTS-1870 — since-last-run findings delta. `delta` and
            // `delta_unavailable_reason` are mutually exclusive; exactly one
            // appears under a narrowed since-last-run, neither otherwise.
            // `findings_truncated` flags a run that hit the per-tool finding
            // ceiling (the delta is then suppressed in favour of the reason).
            if (!r.delta.isEmpty())
                env["delta"] = r.delta;
            if (!r.deltaUnavailableReason.isEmpty())
                env["delta_unavailable_reason"] = r.deltaUnavailableReason;
            if (r.findingsTruncated)
                env["findings_truncated"] = true;
            return QString::fromUtf8(
                QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    // ANTS-1397 — test_audit trio (v1). All four verbs Optional
    // contract per § 2.4 (matches cold_eyes / indie_review /
    // debt_sweep). fold_in delegates to RoadmapFoldIn::* engine
    // entries directly (NOT MCP re-entry — INV-3).
    m_claudeIntegration->registerToolProvider("test_audit_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        [](const QJsonObject &args) -> QString {
            TestAuditEngine::PartitionRequest req;
            req.callerCwd   = args.value(QStringLiteral("caller_cwd")).toString();
            req.scope       = args.value(QStringLiteral("scope")).toString();
            req.dimensions  = args.value(QStringLiteral("dimensions")).toString();
            if (args.value(QStringLiteral("chunk_size")).isDouble())
                req.chunkSize = args.value(QStringLiteral("chunk_size")).toInt();
            req.quick       = args.value(QStringLiteral("quick")).toBool();
            if (args.value(QStringLiteral("offset")).isDouble())
                req.offset = args.value(QStringLiteral("offset")).toInt();
            if (args.value(QStringLiteral("limit")).isDouble())
                req.limit = args.value(QStringLiteral("limit")).toInt();
            const auto r = TestAuditEngine::partition(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"] = true;
            env["framework"]    = r.framework;
            // ANTS-1623 — polyglot signal. Only emitted when non-empty
            // so single-framework projects (the common case) carry
            // zero overhead in the envelope.
            if (!r.additionalFrameworks.isEmpty())
                env["additional_frameworks"] = r.additionalFrameworks;
            env["test_globs"]   = QJsonArray::fromStringList(r.testGlobs);
            env["total_files"]  = r.totalFiles;
            env["chunks_count"] = r.chunksCount;
            QJsonArray chunks;
            for (const auto &c : r.chunks) {
                QJsonObject co;
                co["id"]                  = c.id;
                co["paths"]               = QJsonArray::fromStringList(c.paths);
                // ANTS-1487: renamed from `dimension_hints` so callers can't
                // mistake "dimensions the pre-pass grep hit" for "dimensions
                // worth auditing". Full lane list is `dimensions_active` at
                // envelope level.
                co["pre_pass_dimensions"] = QJsonArray::fromStringList(c.prePassDimensions);
                chunks.append(co);
            }
            env["chunks"] = chunks;
            env["dimensions_active"] = QJsonArray::fromStringList(r.dimensionsActive);
            QJsonObject prePass;
            for (auto it = r.prePassFindingsByChunk.constBegin();
                 it != r.prePassFindingsByChunk.constEnd(); ++it) {
                prePass[it.key()] = it.value();
            }
            // ANTS-2070 — the inlined pre-pass map is the envelope's bulk
            // (each chunk caps at 20 findings, but a 35-chunk suite still
            // overflowed the MCP tool-result token cap with 547 findings).
            // When the map would be large, omit it from the wire and flag
            // pre_pass_cached so the caller fetches per-chunk via
            // test_audit_brief — the full map stays in the partition cache
            // for that lookup, and pre_pass_chunk_ids below still advertises
            // which chunks carry findings.
            const QByteArray prePassJson =
                QJsonDocument(prePass).toJson(QJsonDocument::Compact);
            constexpr int kPrePassInlineCapBytes = 24 * 1024;
            const bool prePassOmittedBySize =
                prePassJson.size() > kPrePassInlineCapBytes;
            // ANTS-2096 — a paginated (page 2+) result keeps its pre-pass
            // map in the partition cache for test_audit_brief, but must NOT
            // inline it here: prePassCached signals "fetch per-chunk via
            // brief", so omit the wire map when cached, not only on size.
            if (!prePassOmittedBySize && !r.prePassCached)
                env["pre_pass_findings_by_chunk"] = prePass;
            // ANTS-1489 — echo the chunk-ID keyset at envelope level so
            // callers can decide which per-chunk briefs are worth
            // fetching without descending into the nested map.
            QJsonArray prePassChunkIds;
            for (auto it = r.prePassFindingsByChunk.constBegin();
                 it != r.prePassFindingsByChunk.constEnd(); ++it) {
                if (!it.value().isEmpty()) prePassChunkIds.append(it.key());
            }
            // Stable order — callers may iterate the array directly.
            QStringList idsSorted;
            for (const auto &v : prePassChunkIds) idsSorted.append(v.toString());
            std::sort(idsSorted.begin(), idsSorted.end());
            env["pre_pass_chunk_ids"] = QJsonArray::fromStringList(idsSorted);
            env["pre_pass_cached"] = r.prePassCached || prePassOmittedBySize;
            if (prePassOmittedBySize) {
                // ANTS-2070 — tell the caller why the map is absent and how
                // big it was, so it knows to fetch per-chunk via brief.
                env["pre_pass_omitted"] = true;
                env["pre_pass_omitted_bytes"] = prePassJson.size();
            }
            env["partition_token"] = r.partitionToken;
            env["offset"]    = r.offset;
            env["limit"]     = r.limit;
            env["total"]     = r.total;
            env["truncated"] = r.truncated;
            if (r.nextOffset >= 0) env["next_offset"] = r.nextOffset;
            env["byte_count"] = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("test_audit_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        [](const QJsonObject &args) -> QString {
            TestAuditEngine::BriefRequest req;
            req.callerCwd       = args.value(QStringLiteral("caller_cwd")).toString();
            req.chunkId         = args.value(QStringLiteral("chunk_id")).toString();
            req.partitionToken  = args.value(QStringLiteral("partition_token")).toString();
            const auto r = TestAuditEngine::brief(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                = true;
            env["chunk_id"]          = r.chunkId;
            env["source_paths"]      = QJsonArray::fromStringList(r.sourcePaths);
            env["dimensions"]        = QJsonArray::fromStringList(r.dimensions);
            env["framework_context"] = r.frameworkContext;
            env["pre_pass_findings"] = r.prePassFindings;
            // ANTS-1457 — surface the prior false-positive ledger
            // entries as a structured field for the reviewer LLM.
            env["prior_false_positives"] = r.priorFalsePositives;
            env["byte_count"]        = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("test_audit_synthesis_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        [](const QJsonObject &args) -> QString {
            TestAuditEngine::SynthRequest req;
            req.callerCwd          = args.value(QStringLiteral("caller_cwd")).toString();
            req.partitionToken     = args.value(QStringLiteral("partition_token")).toString();
            req.reportsDir         = args.value(QStringLiteral("reports_dir")).toString();
            req.calibrationAnchor  = args.value(QStringLiteral("calibration_anchor")).toObject();
            // ANTS-1455 — opt-in escape hatch + mode + pagination.
            req.allowOutsideProject = args.value(QStringLiteral("allow_outside_project")).toBool(false);
            req.mode               = args.value(QStringLiteral("mode")).toString();
            req.offset             = args.value(QStringLiteral("offset")).toInt(0);
            // limit defaulting: if caller omitted, leave at -1 sentinel
            // so engine picks mode-appropriate default (5 for "full",
            // ignored for "summary"). 0 is a valid "use default" too.
            if (args.contains(QStringLiteral("limit"))) {
                req.limit = args.value(QStringLiteral("limit")).toInt(-1);
            }
            const auto r = TestAuditEngine::synthesize(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                  = true;
            env["mode"]                = r.mode;
            env["prompt"]              = r.prompt;
            env["dimension_summaries"] = r.dimensionSummaries;
            env["top_dimensions"]      = r.topDimensions;
            env["file_index"]          = r.fileIndex;
            // ANTS-1488 — per-dimension severity histograms so callers
            // can decide whether to drop into mode:"full" or mode:"hybrid"
            // based on whether any dimension surfaced a CRIT/HIGH.
            env["severity_histograms"] = r.severityHistograms;
            env["truncated"]           = r.truncated;
            env["reports_read"]        = r.reportsRead;
            env["chunks_total"]        = r.chunksTotal;
            env["chunks_returned"]     = r.chunksReturned;
            env["next_offset"]         = r.nextOffset;
            env["truncated_by_limit"]  = r.truncatedByLimit;
            env["byte_count"]          = r.byteCount;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("test_audit_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        [](const QJsonObject &args) -> QString {
            TestAuditEngine::FoldInRequest req;
            req.callerCwd    = args.value(QStringLiteral("caller_cwd")).toString();
            req.actionable   = args.value(QStringLiteral("actionable")).toArray();
            req.framework    = args.value(QStringLiteral("framework")).toString();
            req.filesScanned = args.value(QStringLiteral("files_scanned")).toInt();
            const QJsonArray dimsArr = args.value(QStringLiteral("dimensions")).toArray();
            for (const QJsonValue &v : dimsArr) req.dimensions.append(v.toString());
            req.rawFindings  = args.value(QStringLiteral("raw_findings")).toInt();
            // ANTS-1635 — narrative-mode opt-in. Forward both fields so
            // the engine's short-circuit gate is reachable.
            req.narrativeMode = args.value(QStringLiteral("narrative_mode")).toBool();
            req.narrativeMd   = args.value(QStringLiteral("narrative_md")).toString();
            const auto r = TestAuditEngine::foldIn(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                env["written_count"]=r.writtenCount; env["failed_count"]=r.failedCount;
                env["partial"]=r.partial;
                // ANTS-1527 — surface counter_path as a programmatic
                // field on id_counter_failed so the caller can clear
                // a stale `.lock` sibling without parsing the prose
                // error. Only emitted when the engine populated it
                // (id_counter_failed path); other failure modes leave
                // it empty.
                if (!r.counterPath.isEmpty()) env["counter_path"] = r.counterPath;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]                    = true;
            env["block"]                 = r.block;
            env["allocated_ids"]         = QJsonArray::fromStringList(r.allocatedIds);
            env["written"]               = r.written;
            env["release_block_heading"] = r.releaseBlockHeading;
            env["bytes_written"]         = r.bytesWritten;
            env["written_count"]         = r.writtenCount;
            env["failed_count"]          = r.failedCount;
            env["partial"]               = r.partial;
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    // ANTS-1513 — test_audit_recheck: verify a deferred finding's cite
    // is still live before resuming the work. Read-only project query.
    m_claudeIntegration->registerToolProvider("test_audit_recheck",
        ClaudeIntegration::CallerCwdContract::Required,
        [](const QJsonObject &args) -> QString {
            TestAuditEngine::RecheckRequest req;
            req.callerCwd  = args.value(QStringLiteral("caller_cwd")).toString();
            req.findingId  = args.value(QStringLiteral("finding_id")).toString();
            const auto r = TestAuditEngine::recheck(req);
            QJsonObject env;
            if (!r.ok) { env["ok"]=false; env["code"]=r.code; env["error"]=r.error;
                return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)); }
            env["ok"]    = true;
            env["found"] = r.found;
            if (r.found) {
                env["cited_file"]  = r.citedFile;
                env["cited_line"]  = r.citedLine;
                env["file_exists"] = r.fileExists;
                env["line_exists"] = r.lineExists;
                if (r.lineExists) {
                    env["current_line_text"]         = r.currentLineText;
                    env["line_still_matches_pattern"] = r.lineStillMatchesPattern;
                    if (r.lineStillMatchesPattern) {
                        env["matched_pattern_id"] = r.matchedPatternId;
                        env["matched_dimension"]  = r.matchedDimension;
                    }
                }
                // ANTS-1513 — best-effort git rename hint for a gone file.
                // Lives here (not the engine) so testauditengine.cpp stays
                // QProcess-free (test_audit trio INV-1). Only for a
                // relative cite under an existing project root.
                if (!r.fileExists && !r.citedFile.isEmpty() &&
                    !r.citedFile.startsWith(QLatin1Char('/'))) {
                    const QString canon = QFileInfo(
                        args.value(QStringLiteral("caller_cwd")).toString())
                        .canonicalFilePath();
                    if (!canon.isEmpty()) {
                        QProcess git;
                        git.setWorkingDirectory(canon);
                        git.start(QStringLiteral("git"), {
                            QStringLiteral("log"), QStringLiteral("--all"),
                            QStringLiteral("--diff-filter=R"),
                            QStringLiteral("--name-status"),
                            QStringLiteral("--format="), QStringLiteral("--"),
                            r.citedFile });
                        if (git.waitForFinished(3000) &&
                            git.exitStatus() == QProcess::NormalExit) {
                            const QStringList outLines = QString::fromUtf8(
                                git.readAllStandardOutput())
                                .split(QChar('\n'), Qt::SkipEmptyParts);
                            for (const QString &ol : outLines) {
                                if (!ol.startsWith(QChar('R'))) continue;
                                const QStringList parts = ol.split(QChar('\t'));
                                if (parts.size() >= 3) {
                                    env["drift_hint"] = QStringLiteral(
                                        "file likely moved to %1")
                                        .arg(parts.at(2));
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            return QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact));
        });
    m_claudeIntegration->registerToolProvider("get_text",
        ClaudeIntegration::CallerCwdContract::TabSpecific,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            // ANTS-1244 INV-9 — tab=0 is a valid index distinct from
            // "tab omitted"; gate via isDouble(). Same shape for lines
            // (matches the IPC verb at remotecontrol.cpp:347).
            QJsonObject req;
            if (args.value("tab").isDouble())   req["tab"]   = args.value("tab").toInt();
            if (args.value("lines").isDouble()) req["lines"] = args.value("lines").toInt();
            // ANTS-1392 — forward caller_cwd so cmdGetText's
            // tab-resolution falls through to terminalForCaller when
            // `tab` is omitted.
            const QJsonValue cwdVal = args.value("caller_cwd");
            if (cwdVal.isString() && !cwdVal.toString().isEmpty())
                req["caller_cwd"] = cwdVal.toString();
            return QString::fromUtf8(
                m_remoteControl->cmdGetText(req).toJson(QJsonDocument::Compact));
        });
    // ANTS-2144 — off the socket thread: cmdWorkspaceSearch blocks on
    // rg.waitForFinished(), which starved the QLocalSocket notifier and
    // tripped concurrent verbs into a -32000 transport timeout. caller_cwd
    // is Required here, so the off-thread path never reaches the
    // m_main->currentTerminal() fallback (main-thread-only state).
    m_claudeIntegration->registerToolProvider("workspace_search",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdWorkspaceSearch));
    m_claudeIntegration->registerToolProvider("file_outline",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFileOutline));
    // ANTS-1855 — read_log: filter a log file (debug log or caller_cwd path).
    m_claudeIntegration->registerToolProvider("read_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdReadLog));
    // ANTS-2021 — read_region: line-range / symbol-body slice of a project file.
    m_claudeIntegration->registerToolProvider("read_region",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdReadRegion));
    // ANTS-2219 — read_regions: batched multi-selector read (read-side mirror
    // of apply_edits). Per-item etag → individual 304; shared max_bytes budget.
    m_claudeIntegration->registerToolProvider("read_regions",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdReadRegions));
    // ANTS-2094 — read_spill: re-read an offloaded result by its handle.
    // caller_cwd Optional — the spill store is global/content-addressed,
    // not project-scoped.
    m_claudeIntegration->registerToolProvider("read_spill",
        ClaudeIntegration::CallerCwdContract::Optional,
        rcDelegate(&RemoteControl::cmdReadSpill));
    // ANTS-2022 — apply_edits: atomic-per-file batch of {path, old, new} edits.
    m_claudeIntegration->registerToolProvider("apply_edits",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdApplyEdits));
    // ANTS-1637 — codebase_index: pre-computed project structural map.
    m_claudeIntegration->registerToolProvider("codebase_index",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdCodebaseIndex));
    // ANTS-2139 — docs_index: pre-computed project documentation map.
    m_claudeIntegration->registerToolProvider("docs_index",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdDocsIndex));
    // ANTS-2161 — project_settings: detect layout + create/update .ants/project.json.
    m_claudeIntegration->registerToolProvider("project_settings",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdProjectSettings));
#ifdef ANTS_LUA_PLUGINS
    // ANTS-2093 — project_query: run an agent-supplied read-only Lua snippet
    // server-side and return only its result (the code-execution token-saver).
    // Lives entirely in ants_lua_lib (LuaEngine::projectQueryVerb) because
    // ants_core_lib's RemoteControl cannot see LuaEngine; the provider lambda
    // (chrome_lib, which links lua_lib) reads the gate + tuning from Config and
    // delegates. Registered only in ANTS_LUA_PLUGINS builds (verb absent
    // otherwise — clean drop-out, no dead refusal path). See docs/specs/ANTS-2093.md.
    m_claudeIntegration->registerToolProvider("project_query",
        ClaudeIntegration::CallerCwdContract::Required,
        [this](const QJsonObject &args) -> QString {
            return QString::fromUtf8(QJsonDocument(LuaEngine::projectQueryVerb(
                    args,
                    m_config.claudeMcpProjectQueryEnabled(),
                    m_config.claudeMcpProjectQueryTimeoutMs(),
                    m_config.claudeMcpProjectQueryResultCapBytes()))
                .toJson(QJsonDocument::Compact));
        });
#endif
    // ANTS-1961 / ANTS-1962 — cross-session feedback-file read + write.
    m_claudeIntegration->registerToolProvider("feedback_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFeedbackQuery));
    m_claudeIntegration->registerToolProvider("feedback_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFeedbackLog));
    // ANTS-2129 — audit_falsepos_log: write side of the false-positive ledger.
    m_claudeIntegration->registerToolProvider("audit_falsepos_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdAuditFalseposLog));
    m_claudeIntegration->registerToolProvider("git_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdGitState));
    m_claudeIntegration->registerToolProvider("subsystem",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSubsystem));
    m_claudeIntegration->registerToolProvider("last_audit_summary",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdLastAuditSummary));
    // ANTS-1569 — current_state aggregator. MCP-only (mirrors
    // last_audit_summary; no IPC dispatch branch). Pure composer over
    // cmdRoadmapQuery + cmdGitState + cmdLastAuditSummary.
    m_claudeIntegration->registerToolProvider("current_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdCurrentState));

    // ANTS-1735 — model_switch_stats. Read-only aggregation of the model-switch
    // effectiveness ledger, scoped to caller_cwd's project. MCP-only (mirrors
    // current_state; no IPC dispatch branch). See docs/specs/ANTS-1735.md §2.5.
    m_claudeIntegration->registerToolProvider("model_switch_stats",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdModelSwitchStats));

    // ANTS-1309 + ANTS-1308 — spec-aware token-savers. spec_query
    // returns one spec's parsed {title, status, kind, invariants[]};
    // invariant_check scans docs/specs/*.md for specs that mention
    // any path in `files[]` and returns their invariant lists.
    // Both MCP-only.
    m_claudeIntegration->registerToolProvider("spec_query",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSpecQuery));
    // ANTS-1963 — spec_log: write the three recurring spec mutations.
    m_claudeIntegration->registerToolProvider("spec_log",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSpecLog));
    m_claudeIntegration->registerToolProvider("invariant_check",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdInvariantCheck));

    // ANTS-1306 + ANTS-1307 — task-start context composers.
    // task_priors bundles matching specs + ROADMAP cards + recent
    // commits + ADRs for a free-text task description; project_conventions
    // returns the task_type-scoped convention subset. Both MCP-only.
    // See docs/specs/ANTS-1306.md and docs/specs/ANTS-1307.md.
    m_claudeIntegration->registerToolProvider("task_priors",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdTaskPriors));
    m_claudeIntegration->registerToolProvider("project_conventions",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdProjectConventions));

    // ANTS-1299 + ANTS-1300 — build/test cache MCP tools. Both
    // are op-dispatched (op=read | op=record) and write to
    // <project>/.audit_cache/. MCP-only. See docs/specs/ANTS-1299.md
    // and docs/specs/ANTS-1300.md.
    m_claudeIntegration->registerToolProvider("build_status",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdBuildStatus));
    m_claudeIntegration->registerToolProvider("test_results",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdTestResults));

    // ANTS-1302 — focused_test. Runs only the ctest subset touching the
    // changed files (via tests/coverage-map.json), returns the
    // test_results envelope. Expensive (shells out to ctest), MCP-only.
    // See docs/specs/ANTS-1302.md.
    m_claudeIntegration->registerToolProvider("focused_test",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFocusedTest));

    // ANTS-1303 — find_definition + find_caller. Tree-wide regex symbol
    // scanner (no LSP). Both take {symbol, caller_cwd, lang?,
    // max_results?} and delegate to SymbolQuery via RemoteControl.
    // MCP-only conceptually; also reachable via the IPC dispatch verbs
    // find-definition / find-caller. See docs/specs/ANTS-1303.md.
    m_claudeIntegration->registerToolProvider("find_definition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFindDefinition));
    m_claudeIntegration->registerToolProvider("find_caller",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdFindCaller));

    // ANTS-1305 — similar_code. Tree-wide shape matcher: reuses the
    // FileOutline extractor + ranks signatures by token-set Jaccard
    // similarity to a free-text {shape, caller_cwd, lang?, max_results?}
    // query. Delegates to SimilarCode via RemoteControl. MCP-only
    // conceptually; also reachable via the IPC dispatch verb
    // similar-code. See docs/specs/ANTS-1305.md.
    m_claudeIntegration->registerToolProvider("similar_code",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSimilarCode));

    // ANTS-1112 — five `indie_review_*` tools. Each handler resolves
    // the active project via the focused TerminalWidget's shellCwd
    // (matches the convention used by git_state / subsystem /
    // last_audit_summary). All five delegate to RemoteControl's
    // cmdIndieReview* methods, mirroring the ANTS-1253 registry shape.
    m_claudeIntegration->registerToolProvider("indie_review_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewPartition));
    m_claudeIntegration->registerToolProvider("indie_review_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewBrief));
    m_claudeIntegration->registerToolProvider("indie_review_corroborate",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewCorroborate));
    m_claudeIntegration->registerToolProvider("indie_review_synthesis_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewSynthesisPrompt));
    m_claudeIntegration->registerToolProvider("indie_review_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewFoldIn));
    // ANTS-1279 — indie_review_orchestrate. Pure read (partition + brief
    // manifests); no subprocess, so no in-flight gate. caller_cwd Required.
    m_claudeIntegration->registerToolProvider("indie_review_orchestrate",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdIndieReviewOrchestrate));

    // ANTS-1352 — indie_review_dispatch. Inline in-flight gate via
    // verbInFlight* (same pattern as audit_run); caller_cwd Required
    // per callerCwdContractFor; rate-limit tier Expensive.
    m_claudeIntegration->registerToolProvider("indie_review_dispatch",
        ClaudeIntegration::CallerCwdContract::Required,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            const QString callerCwd = args.value(
                QStringLiteral("caller_cwd")).toString();
            // ANTS-1833 — reject a non-existent root before it can
            // collapse the in-flight key (canon would be empty).
            QString canon, badEnv;
            if (!resolveInflightCallerCwd(callerCwd, "indie_review_dispatch",
                                          &canon, &badEnv))
                return badEnv;
            const qint64 existing =
                m_claudeIntegration->verbInFlightTryAcquire(
                    QStringLiteral("indie_review_dispatch"), canon);
            if (existing >= 0) {
                QJsonObject env;
                env["ok"]               = false;
                env["code"]             = QStringLiteral("already_running");
                env["error"]            = QStringLiteral(
                    "indie_review_dispatch: a sweep is already in flight "
                    "for this project root; retry after it completes");
                env["running_since_ms"] =
                    QDateTime::currentMSecsSinceEpoch() - existing;
                env["retry_after_ms"]   = 30000;
                return QString::fromUtf8(
                    QJsonDocument(env).toJson(QJsonDocument::Compact));
            }
            // RAII: release on every exit path (incl. exception). indie-review-2026-05-21.
            const auto inFlightGuard = qScopeGuard([this, &canon] {
                m_claudeIntegration->verbInFlightRelease(
                    QStringLiteral("indie_review_dispatch"), canon);
            });
            // ANTS-2104 — run the dispatch on a worker thread, exactly like
            // audit_run (ANTS-2103). cmdIndieReviewDispatch -> dispatchLanes
            // spins a local QNetworkAccessManager + QEventLoop (indiereview
            // dispatcher.cpp:223-224); on the main thread that nested loop
            // reentrantly delivers MCP QLocalSocket read-notifications during
            // the multi-minute LLM sweep -> the same use-after-free SIGSEGV
            // class as audit_run. The nam/loop are locals, so they construct
            // on the worker; QThread::wait() is a join (no event pump), so no
            // foreign socket notification fires during the dispatch.
            QJsonDocument doc;
            {
                QThread *worker = QThread::create(
                    [this, &args, &doc]() {
                        doc = m_remoteControl->cmdIndieReviewDispatch(args);
                    });
                worker->start();
                worker->wait();
                delete worker;
            }
            QString out = QString::fromUtf8(
                doc.toJson(QJsonDocument::Compact));
            return out;
        });

    // ANTS-1113 — debt_sweep_* (4 tools). ANTS-2131 — on a worker thread:
    // _scan shells out to git and _apply_fix to a packaging script
    // (debtsweepengine.cpp waitForFinished); _defer/_triage_prompt are fast
    // and in-process but wrapped too, so the whole family is uniform (and
    // future-safe if either grows a shell-out). Worker overhead is a
    // sub-millisecond thread spawn — negligible on these non-hot verbs.
    m_claudeIntegration->registerToolProvider("debt_sweep_scan",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdDebtSweepScan));
    m_claudeIntegration->registerToolProvider("debt_sweep_apply_fix",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdDebtSweepApplyFix));
    m_claudeIntegration->registerToolProvider("debt_sweep_defer",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdDebtSweepDefer));
    m_claudeIntegration->registerToolProvider("debt_sweep_triage_prompt",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdDebtSweepTriagePrompt));

    // ANTS-1289 — verify_changes. ANTS-2131 — on a worker thread: it shells
    // out to per-gate build/test commands (verifyengine.cpp waitForFinished),
    // which would otherwise freeze the GUI for the gate timeout.
    m_claudeIntegration->registerToolProvider("verify_changes",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegateWorker(&RemoteControl::cmdVerifyChanges));

    // ANTS-1290 — plan_template.
    m_claudeIntegration->registerToolProvider("plan_template",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdPlanTemplate));

    // ANTS-1284 — token_usage. ANTS-1422 pull 3: explicit
    // ClaudeIntegration* is now the canonical (only) path. The
    // m_main->claudeIntegration() fallback was retired — it
    // returned null on a live build with no static-analysis
    // explanation, and the only call site (this lambda) always
    // supplies the pointer.
    m_claudeIntegration->registerToolProvider("token_usage",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [this](const QJsonObject &args) -> QString {
            if (!m_remoteControl) return QString::fromUtf8(kRcUnavailable);
            return QString::fromUtf8(
                m_remoteControl->cmdTokenUsage(args, m_claudeIntegration)
                    .toJson(QJsonDocument::Compact));
        });

    // ANTS-1360 — mcp_trace: read a slice of ClaudeIntegration's
    // ring buffer of last tool/call dispatches. Does NOT delegate
    // to RemoteControl — the ring lives inside ClaudeIntegration.
    m_claudeIntegration->registerToolProvider("mcp_trace",
        ClaudeIntegration::CallerCwdContract::ProcessGlobal,
        [ci = m_claudeIntegration](const QJsonObject &args) -> QString {
            const quint64 since = static_cast<quint64>(
                args.value("since").toVariant().toLongLong());
            const int limit = args.value("limit").toInt(50);
            return QString::fromUtf8(
                QJsonDocument(ci->queryMcpTrace(since, limit))
                    .toJson(QJsonDocument::Compact));
        });

    // ANTS-1319 — cold_eyes_* (4 tools). Mirror to indie_review fold-in
    // pattern; each handler delegates to RemoteControl::cmdColdEyes*.
    m_claudeIntegration->registerToolProvider("cold_eyes_partition",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdColdEyesPartition));
    m_claudeIntegration->registerToolProvider("cold_eyes_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdColdEyesBrief));
    m_claudeIntegration->registerToolProvider("cold_eyes_cross_doc_diff",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdColdEyesCrossDocDiff));
    m_claudeIntegration->registerToolProvider("cold_eyes_fold_in",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdColdEyesFoldIn));
    // ANTS-1413 — cold_eyes_single_doc. Single-spec cross-consistency
    // brief without the partition+brief multi-step.
    m_claudeIntegration->registerToolProvider("cold_eyes_single_doc",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdColdEyesSingleDoc));
    // ANTS-1414 — cross_doc_diff. Lane-source-agnostic alias for the
    // regex hotspot primitive shared by cold-eyes + indie-review.
    m_claudeIntegration->registerToolProvider("cross_doc_diff",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdCrossDocDiff));

    // ANTS-1283 — session_memory KV.
    m_claudeIntegration->registerToolProvider("session_memory",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSessionMemory));

    // ANTS-1723 — workflow_state: superpowers skill step/phase store.
    m_claudeIntegration->registerToolProvider("workflow_state",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdWorkflowState));

    // ANTS-1724 — session_brief: compact session-state envelope.
    m_claudeIntegration->registerToolProvider("session_brief",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSessionBrief));

    // ANTS-1883 — session_orient: bundle of current_state +
    // project_layout + roadmap_query (section_index, active).
    m_claudeIntegration->registerToolProvider("session_orient",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdSessionOrient));

    // ANTS-1430 — project_layout pre-cache.
    m_claudeIntegration->registerToolProvider("project_layout",
        ClaudeIntegration::CallerCwdContract::Required,
        rcDelegate(&RemoteControl::cmdProjectLayout));

    // ANTS-1400 — caller_cwd_info diagnostic verb. Pure delegation to
    // ants::resolveCallerCwdRoot. No filesystem operations beyond the
    // canonicalisations the helper performs; no shell, no process.
    // The verb is intentionally classified Optional in
    // ClaudeIntegration::callerCwdContractFor so empty caller_cwd is
    // accepted (EmptyFallback is the legitimate "what would happen
    // without it?" question). See docs/specs/ANTS-1400.md.
    m_claudeIntegration->registerToolProvider("caller_cwd_info",
        ClaudeIntegration::CallerCwdContract::Optional,
        [this](const QJsonObject &args) -> QString {
            const QString callerCwd =
                args.value(QStringLiteral("caller_cwd")).toString();
            const ants::ResolvedRoot rr =
                ants::resolveCallerCwdRoot(this, callerCwd);
            QJsonObject env;
            env["ok"]           = true;
            env["source"]       = sourceToString(rr.source);
            env["resolved_cwd"] = rr.cwd;
            if (rr.tabIndex) env["tab_index"] = *rr.tabIndex;
            return QString::fromUtf8(
                QJsonDocument(env).toJson(QJsonDocument::Compact));
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
    }
    // ANTS-1168: refresh on every open, including the first. The prior
    // construction-only branch left the first show stale until the user
    // closed and re-opened.
    m_claudeProjects->refresh();

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
    // ANTS-1363 — pause the 2 s status-poll timer while Ants is not the
    // active window. updateStatusBar(), the chip refreshes and the
    // autonomous-switcher gate all run on this tick; an unfocused Ants in a
    // background workspace otherwise pays per-tick CPU + scheduler wakeups for
    // UI nobody is looking at (battery cost on laptops). The 2 s freshness
    // bound (ANTS-1219-INV-2 / ANTS-1160 §9) only governs config/resolver
    // swaps, which can only originate while the window is focused, so pausing
    // here preserves the contract — the timer restarts on re-activation.
    if (m_statusTimer) {
        if (event->type() == QEvent::WindowActivate) {
            if (!m_statusTimer->isActive()) m_statusTimer->start();
        } else if (event->type() == QEvent::WindowDeactivate) {
            m_statusTimer->stop();
        }
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
    // ANTS-1051: pseudo-modal blocking. When any QDialog is visible,
    // mouse/key/wheel events that land outside its tree get
    // suppressed — emulating the click-blocking semantics of
    // setModal(true) without tripping QTBUG-79126 on
    // KDE+KWin+Qt6.11+frameless. Pure-logic helper in
    // src/dialogfocus.h so the feature test drives it without a
    // real MainWindow. Returns true to swallow the event; the
    // helper's mutual-exclusivity on event type with
    // shouldRefocusOnDialogClose (one fires on Close, the other
    // on mouse/key) means dispatch order here is immaterial.
    if (dialogfocus::shouldSuppressEventForDialog(watched, event)) {
        return true;  // swallow — Qt convention: true = consumed.
    }

    // ANTS-1050: auto-return focus to the active terminal when any
    // dialog closes. The dialogfocus::shouldRefocusOnDialogClose
    // helper is pure logic in src/dialogfocus.h so the feature test
    // exercises it without constructing a real MainWindow. The
    // deferred dispatch (singleShot(0)) lets the dialog finish its
    // teardown before we grab focus. The null-guard on
    // focusedTerminal() defends against early-startup dialogs
    // (config-load failure) that close before any terminal exists.
    if (dialogfocus::shouldRefocusOnDialogClose(watched, event)) {
        QPointer<MainWindow> self(this);
        QTimer::singleShot(0, this, [self]() {
            if (!self) return;
            if (auto *t = self->focusedTerminal()) {
                t->setFocus(Qt::OtherFocusReason);
            }
        });
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    // ANTS-1159 — stop the periodic session-save timer first so
    // a tick landing mid-shutdown can't re-enter the save path
    // and race the explicit shutdown save below.
    if (m_sessionSaveTimer) m_sessionSaveTimer->stop();

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
    if (m_claudeStatusBarController) m_claudeStatusBarController->clearError();
    // ANTS-1852 — tear down the permission-prompt anchors, but keep a
    // background tab's still-pending anchor alive (hidden) so its retraction
    // wiring survives to clear the dot if that prompt resolves while another
    // tab is focused. The focused tab's anchor is still destroyed and rebuilt
    // fresh by maybeShowPromptForActiveTab below. Replaces the old blanket
    // findChildren->deleteLater. `t` is the tab being switched to (null
    // mid-teardown → delete everything).
    if (m_claudeStatusBarController)
        m_claudeStatusBarController->clearPromptAnchorsForTabSwitch(
            t ? t->shellPid() : 0);
    if (m_claudeStatusBarController)
        m_claudeStatusBarController->setPromptActive(false);

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
        // ANTS-1146 — single atomic reset covers the five state
        // booleans, three widget hides, and bg-tasks transcript path
        // clear that this block used to enumerate inline.
        // ANTS-1219-INV-4: tab-change call site for resetForTabSwitch.
        // Removing this re-introduces cross-tab task bleed (old tab's
        // path stays bound after switch).
        if (m_claudeStatusBarController)
            m_claudeStatusBarController->resetForTabSwitch();
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
    //     flow into the controller via the existing connection. State
    //     is CLEARED inside setShellPid when the PID changes, so the
    //     label never carries over from the previous tab. See
    //     claudeintegration.cpp:58-71.
    //   - refreshReviewButton() spawns an async `git status` probe;
    //     the button is hidden immediately and revealed only when the
    //     probe confirms the new tab's cwd is a git repo.
    if (m_claudeIntegration)
        m_claudeIntegration->setShellPid(t->shellPid());
    // Plan / auditing flags are derived from the transcript and will
    // be refreshed by the next ClaudeIntegration stateChanged signal,
    // but clear them now so the wrong-tab's flags don't briefly show
    // until that signal arrives.
    if (m_claudeStatusBarController) {
        m_claudeStatusBarController->setPlanMode(false);
        m_claudeStatusBarController->setAuditing(false);
    }
    updateStatusBar();
    refreshReviewButton();
    if (m_claudeStatusBarController)
        m_claudeStatusBarController->refreshBgTasksButton();
    refreshRoadmapButton();
    refreshRepoVisibility();

    // ANTS-1851 — if the tab we just switched TO owns a still-pending
    // permission prompt, re-paint its bottom-bar Allow/Deny buttons (the
    // Category-C teardown above removed the previous tab's). Runs last so
    // it paints over the cleared message slot, not under it.
    if (m_claudeStatusBarController)
        m_claudeStatusBarController->maybeShowPromptForActiveTab(t->shellPid());
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
        // ANTS-1147 — cache-and-compare guard. Pre-1147 this path
        // re-applied the chip stylesheet on every 2-s tick even when
        // neither theme nor primary-branch flag had changed. Now we
        // compute the QSS via the helper, compare against the cached
        // string, and only call setStyleSheet when it differs. The
        // string itself encodes the (theme × primary × margin) triple
        // so a single string-compare is sufficient. m_lastBranchChipValid
        // covers the first-tick case where the cache is uninitialised.
        const Theme &chipTheme = Themes::byName(m_currentTheme);
        const bool chipPrimary =
            branchchip::isPrimaryBranch(gitBranch);
        const QColor &chipCol =
            chipPrimary ? chipTheme.ansi[2] : chipTheme.ansi[3];
        const QString newQss = themedstylesheet::buildChipStylesheet(
            chipTheme, chipCol, /*leftMarginPx=*/4);
        if (!m_lastBranchChipValid || newQss != m_lastBranchChipQss) {
            m_statusGitBranch->setStyleSheet(newQss);
            m_lastBranchChipQss = newQss;
            m_lastBranchChipValid = true;
        }
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

void MainWindow::wireQuakeHotkey() {
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
    //
    // Idempotent: the !m_gsPortal guard means a second call (e.g. the
    // Settings toggle after the constructor already wired it) won't
    // double-bind the portal. The constructor and the toggle paths are
    // mutually exclusive in practice (the toggle's !m_quakeMode guard),
    // but the helper stays safe if a future caller breaks that.
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
    if (!hotkeyStr.isEmpty() && !m_gsPortal && GlobalShortcutsPortal::isAvailable()) {
        m_gsPortal = new GlobalShortcutsPortal(this);
        connect(m_gsPortal, &GlobalShortcutsPortal::activated, this,
                [this](const QString &id) {
                    if (id != QStringLiteral("toggle-quake")) return;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - m_lastQuakeToggleMs < 500) return;
                    m_lastQuakeToggleMs = now;
                    toggleQuakeVisibility();
                });
        // ANTS-1142 — listen to sessionFailed with a status-bar
        // notification fallback. Pre-fix code emitted
        // sessionFailed to no listener — on GNOME (where
        // CreateSession succeeds but BindShortcuts fails) the
        // user just saw "F12 doesn't work" with no diagnostic.
        // Surface it once, then move on (sessionFailed is
        // terminal-per-process per the post-ANTS-1142 contract).
        connect(m_gsPortal, &GlobalShortcutsPortal::sessionFailed,
                this, [this](const QString &reason) {
            qWarning("GlobalShortcutsPortal::sessionFailed: %s",
                     qUtf8Printable(reason));
            showStatusMessage(
                tr("Global hotkey unavailable — %1").arg(reason),
                6000);
        });
        m_gsPortal->bindShortcut(
            QStringLiteral("toggle-quake"),
            tr("Toggle Ants Terminal drop-down"),
            qtKeySequenceToPortalTrigger(hotkeyStr));
    }
}

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
    QPushButton *reviewBtn = m_claudeStatusBarController
        ? m_claudeStatusBarController->reviewButton() : nullptr;
    if (!reviewBtn) return;

    auto *t = focusedTerminal();
    if (!t) t = currentTerminal();
    if (!t) {
        // No active terminal — button has nothing to review against.
        reviewBtn->hide();
        return;
    }

    const QString cwd = t->shellCwd();
    if (cwd.isEmpty()) {
        reviewBtn->hide();
        return;
    }

    // ANTS-1013 indie-review-2026-04-27: skip the spawn if a previous
    // probe is still alive. The 2 s status timer races against any
    // probe that takes longer than 2 s (cold-cache git status on a
    // big repo, NFS-mounted cwd, etc.); without this guard each tick
    // accumulated processes.
    if (m_reviewProbeInFlight) return;

    // Policy (user spec 2026-04-18):
    //   - Not a git repo                      → hide entirely
    //   - Git repo, clean AND in-sync upstream → visible-but-DISABLED
    //     (shows the user "this tab tracks a repo" without advertising
    //     an action there isn't anything to review). The global
    //     hover-gate CSS rule lives in themedstylesheet::buildAppStylesheet
    //     (post-ANTS-1147; pre-1147 it was inlined here in applyTheme)
    //     and prevents the disabled button from misleadingly lighting
    //     up on hover.
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

    QPointer<QPushButton> btn = reviewBtn;
    QPointer<QProcess> guard = proc;
    QPointer<MainWindow> self(this);
    m_reviewProbeInFlight = true;
    connect(proc, &QProcess::finished, this,
            [btn, guard, self](int exitCode, QProcess::ExitStatus status) {
        if (btn) {
            if (status != QProcess::NormalExit || exitCode == 128) {
                btn->hide();           // not a git repo / git crash
            } else if (exitCode != 0) {
                btn->hide();
            } else {
                const QByteArray raw = guard ? guard->readAllStandardOutput()
                                              : QByteArray();
                // ANTS-1874 — untracked files now count as reviewable.
                // The prior `?? ` carve-out (2026-05-08) is obsolete since
                // ANTS-1886 renders new files in the diff viewer; predicate
                // extracted to ants::parseReviewPorcelain for unit coverage.
                const ants::ReviewButtonState rs = ants::parseReviewPorcelain(raw);
                btn->setEnabled(rs.dirty || rs.ahead);
                btn->show();
            }
        }
        if (auto *p = self.data()) p->m_reviewProbeInFlight = false;
        if (guard) guard->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this,
            [btn, guard, self](QProcess::ProcessError) {
        if (btn) btn->hide();
        if (auto *p = self.data()) p->m_reviewProbeInFlight = false;
        if (guard) guard->deleteLater();
    });
    proc->start();
}

void MainWindow::showBgTasksDialog() {
    ClaudeBgTaskTracker *tracker = m_claudeStatusBarController
        ? m_claudeStatusBarController->bgTasksTracker() : nullptr;
    if (!tracker) return;
    showStatusMessage(QStringLiteral("Background Tasks: opening…"), 1500);
    // Re-target the tracker before opening so the dialog reflects the
    // active tab's session, not whatever the tracker last saw.
    m_claudeStatusBarController->refreshBgTasksButton();
    auto *dlg = new ClaudeBgTasksDialog(tracker, m_currentTheme, this);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::showTaskListDialog() {
    ClaudeTaskListTracker *tracker = m_claudeStatusBarController
        ? m_claudeStatusBarController->tasksTracker() : nullptr;
    if (!tracker) return;
    showStatusMessage(QStringLiteral("Task List: opening…"), 1500);
    // Re-target the tracker before opening so the dialog reflects
    // the focused tab's session, mirroring showBgTasksDialog.
    m_claudeStatusBarController->refreshTasksButton();
    auto *dlg = new ClaudeTaskListDialog(tracker, m_currentTheme, this);
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
    // ANTS-1137 — case-insensitive match against an explicit
    // candidate list instead of QDir::entryInfoList(QDir::Files)
    // which enumerated the entire CWD on every 2 s status-tick.
    // On a directory with thousands of files (node_modules,
    // ~/Downloads, vendored deps) the enumeration was visible
    // UI stutter. The N QFileInfo::exists() calls remain O(1).
    //
    // ANTS-1459 — same docs/ / docs/private/ / docs/internal/ /
    // .github/ widening as the roadmap_query MCP handler so the
    // status-bar button surfaces on projects (RetroArch et al.)
    // that don't keep ROADMAP.md at the repo root.
    QString found;
    const QStringList candidates = {
        QStringLiteral("ROADMAP.md"),
        QStringLiteral("roadmap.md"),
        QStringLiteral("Roadmap.md"),
        QStringLiteral("docs/ROADMAP.md"),
        QStringLiteral("docs/roadmap.md"),
        QStringLiteral("docs/private/ROADMAP.md"),
        QStringLiteral("docs/private/roadmap.md"),
        QStringLiteral("docs/internal/ROADMAP.md"),
        QStringLiteral("docs/internal/roadmap.md"),
        QStringLiteral(".github/ROADMAP.md"),
        QStringLiteral(".github/roadmap.md"),
    };
    for (const QString &name : candidates) {
        const QString candidate = cwd + QLatin1Char('/') + name;
        if (QFileInfo::exists(candidate)) {
            found = candidate;
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
    auto *dlg = new RoadmapDialog(m_roadmapPath, m_currentTheme,
                                  this, &m_config);
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

    // ANTS-1137 — in-flight guard mirroring m_reviewProbeInFlight.
    // Without this, a fast tab-switch could race two `gh repo view`
    // QProcesses against the same repoRoot, both writing into the
    // same cache slot — winner is order-dependent. Drop redundant
    // probes; cached visibility is still rendered if available.
    if (m_repoVisibilityProbeInFlight.value(repoRoot, false)) return;

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
        // ANTS-1147 — chip QSS shared with the git-branch label via
        // themedstylesheet::buildChipStylesheet. The only delta from
        // the branch chip is the left margin: 0 here so the badge sits
        // flush against the title bar (the branch chip uses 4 px to
        // pair with the git separator). Foreground colour stays
        // public-green / private-red for the at-a-glance visibility
        // cue from 0.7.45.
        m_repoVisibilityLabel->setStyleSheet(
            themedstylesheet::buildChipStylesheet(th, col, /*leftMarginPx=*/0));
        m_repoVisibilityLabel->setToolTip(tr("%1 on GitHub").arg(repoSlug));
        m_repoVisibilityLabel->show();
    };

    // Cache hit (10 min TTL) → render immediately, skip the shell-out.
    constexpr qint64 kCacheTtlMs = 10 * 60 * 1000;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
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
    // `slug` is read verbatim from the repo's .git/config origin URL, so a
    // hostile clone with an origin like `https://github.com/-x/y` could hand
    // `gh` a leading-dash arg parsed as a flag. Guard with a trailing `--`
    // end-of-options sentinel AFTER the flags (gh/Cobra treats everything past
    // `--` as positional, so the `--json`/`-q` flags must precede it; slug then
    // parses as the positional repo even with a leading dash).
    proc->setArguments({QStringLiteral("repo"), QStringLiteral("view"),
                        QStringLiteral("--json"),
                        QStringLiteral("visibility"),
                        QStringLiteral("-q"),
                        QStringLiteral(".visibility"),
                        QStringLiteral("--"), slug});
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
                    QDateTime::currentMSecsSinceEpoch()};
                // ANTS-1137 — clear in-flight on completion regardless
                // of success/failure path. ANTS-1554: tightly-scoped
                // pragma suppresses a GCC -Wnull-dereference false
                // positive — the warning fires inside QHash::isEmpty()
                // (qhash.h:966 `!d || d->size == 0`) when the
                // `QHash::remove` → `removeImpl` → `isEmpty` inline
                // chain is reached from this lambda-via-signal callsite;
                // the short-circuit is logically safe but the
                // template-instantiation context defeats GCC's value
                // tracking.
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
                self->m_repoVisibilityProbeInFlight.remove(repoRoot);
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
                applyVisibility(visibility, slug);
            });
    // ANTS-1137 — mark in-flight before start so a re-entry within
    // this 2 s tick (or any fast tab-switch before the QProcess
    // completes) drops the redundant probe.
    m_repoVisibilityProbeInFlight[repoRoot] = true;
    proc->start();
}

void MainWindow::checkForUpdates(bool userInitiated) {
    if (!m_updateAvailableAction) return;
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
        if (!win->m_updateAvailableAction) return;
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
            win->m_updateAvailableAction->setVisible(false);
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
        // Plain QAction text (no rich-text — menu bars render the
        // string verbatim). The leading ↗ keeps the call-to-action
        // glyph the user is used to from the status-bar variant.
        win->m_updateAvailableAction->setText(
            win->tr("↗ Update v%1 available").arg(tag));
        win->m_updateAvailableAction->setToolTip(
            win->tr("Click to open release notes for v%1 in your browser. "
                    "Currently running v%2.").arg(tag, current));
        win->m_updateAvailableAction->setData(url);
        win->m_updateAvailableAction->setVisible(true);
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
        //
        // 0.7.52 (2026-04-27 indie-review CRITICAL) — was a
        // QMessageBox::exec() (implicit modal + nested event loop)
        // which is exactly the QTBUG-79126 / QTBUG-90005 click-drop
        // shape that the 0.7.50 About-dialog fix retired. On
        // KDE/KWin + Wayland + frameless+translucent parent the
        // user clicks Update and *nothing happens* — the modal-grab
        // handler eats the click. Mirror the same non-modal +
        // plain QPushButton + clicked→close pattern: dialog is
        // heap+WA_DeleteOnClose+show()+raise()+activateWindow();
        // Update click runs the spawn-updater path on close, Cancel
        // click just closes. See debug_wayland_modal_dialog.md memory.
        auto *dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(tr("Update Ants Terminal"));
        dlg->setObjectName(QStringLiteral("updateConfirmDialog"));

        auto *layout = new QVBoxLayout(dlg);
        auto *headline = new QLabel(
            tr("<b>Download and install the new version now?</b>"), dlg);
        auto *body = new QLabel(
            tr("AppImageUpdate will fetch the new release and write "
               "it alongside this binary in the background.<br><br>"
               "To start using the new version you'll need to "
               "<b>quit and re-launch</b> Ants Terminal — any active "
               "Claude Code sessions in your tabs will be "
               "disconnected when you do, and will need to be "
               "reconnected after the restart."), dlg);
        body->setWordWrap(true);
        body->setTextFormat(Qt::RichText);
        layout->addWidget(headline);
        layout->addWidget(body);

        auto *btnRow = new QHBoxLayout;
        btnRow->addStretch();
        auto *cancelBtn = new QPushButton(tr("Cancel"), dlg);
        cancelBtn->setObjectName(QStringLiteral("updateCancelButton"));
        connect(cancelBtn, &QPushButton::clicked, dlg, &QDialog::close);
        connect(cancelBtn, &QPushButton::clicked, this, [this]() {
            showStatusMessage(tr("Update cancelled."), 3000);
        });
        auto *updateBtn = new QPushButton(tr("Update"), dlg);
        updateBtn->setObjectName(QStringLiteral("updateConfirmButton"));
        updateBtn->setDefault(true);
        updateBtn->setAutoDefault(true);
        connect(updateBtn, &QPushButton::clicked, dlg, &QDialog::close);
        connect(updateBtn, &QPushButton::clicked, this,
                [this, updater, appimagePath, url]() {
            // Detached spawn — the updater outlives this binary so
            // the user can quit and restart while the download runs.
            // Qt 6 form: static startDetached(program, args). Returns
            // true on successful fork; we surface the outcome via the
            // status bar rather than another modal dialog.
            const bool ok = QProcess::startDetached(
                updater, QStringList{appimagePath});
            if (ok) {
                showStatusMessage(
                    tr("AppImageUpdate launched — downloading the new "
                       "version. Quit and restart to use it."),
                    8000);
                return;
            }
            // Fork failed — fall back to browser so the user isn't
            // left without recourse.
            showStatusMessage(
                tr("AppImageUpdate failed to launch — opening release "
                   "page in browser instead."),
                5000);
            QDesktopServices::openUrl(QUrl(url));
        });
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(updateBtn);
        layout->addLayout(btnRow);

        dlg->show();
        dlg->raise();
        dlg->activateWindow();
        return;
    }

    // Fallback: open the release page in the user's default browser.
    // QDesktopServices::openUrl is the Qt 6 idiom; it dispatches to
    // xdg-open under XDG, the Win32 ShellExecute equivalent on
    // Windows, and `open` on macOS.
    QDesktopServices::openUrl(QUrl(url));
}

void MainWindow::showDiffViewer() {
    // Thin entry-point. Dialog body lives in diffviewer.{cpp,h}
    // (carved out in 0.7.73 / ANTS-1145). MainWindow keeps the
    // contextual responsibilities: status message, focused-terminal
    // lookup, "Review Changes" button gating, and post-close
    // refresh of the button's enabled state.
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

    // Block re-entry via the button. Re-enable when the dialog is
    // destroyed (WA_DeleteOnClose → destroyed signal fires after
    // the widget tears down). Using destroyed() rather than
    // finished()/closeEvent lets us catch all close paths —
    // window-manager X button, Escape, Alt-F4, Close button —
    // without having to wire each one individually.
    QPushButton *reviewBtn = m_claudeStatusBarController
        ? m_claudeStatusBarController->reviewButton() : nullptr;
    if (reviewBtn) reviewBtn->setEnabled(false);
    QPointer<QPushButton> reviewBtnGuard(reviewBtn);

    QDialog *dialog = diffviewer::show(this, cwd, m_currentTheme);

    connect(dialog, &QObject::destroyed, this, [this, reviewBtnGuard]() {
        if (reviewBtnGuard) {
            reviewBtnGuard->setEnabled(true);
        }
        // Re-run refreshReviewButton so the enabled state reflects
        // the current git state (may have flipped during the time
        // the dialog was open).
        refreshReviewButton();
    });
}


// --- Hot-Reload Configuration ---

void MainWindow::onConfigFileChanged(const QString &path) {
    // Re-entrancy guard. The 0.7.31 attempt at loop prevention was
    // m_configWatcher->blockSignals(true/false) bracketing this slot, but
    // that doesn't work: any save() call inside the reload path (e.g.
    // applyTheme -> setTheme -> save) writes config.json synchronously,
    // which queues a kernel inotify event. Qt only reads that event after
    // this slot returns — by which time blockSignals(false) has already
    // run, so the next fileChanged sails through and re-enters the slot
    // in an infinite loop (status bar sticks at "Config reloaded from
    // disk", the cached settings dialog is repeatedly deleteLater'd, and
    // any other showStatusMessage call gets stomped within milliseconds).
    //
    // Two-layer fix:
    //   1. Setters compare-then-write (Config::setTheme et al.) so a
    //      reload that doesn't change values writes nothing. Primary fix.
    //   2. This re-entrancy flag with deferred clear, in case a future
    //      setter forgets to be idempotent. Defense-in-depth.
    if (m_inConfigReload) return;
    m_inConfigReload = true;

    // Re-add the watch (QFileSystemWatcher drops the watch after some changes)
    if (!m_configWatcher->files().contains(path))
        m_configWatcher->addPath(path);

    // Self-write short-circuit. The Settings dialog (and other in-app
    // writers) mutate THIS same Config object and save() to disk, which
    // trips this very watcher. Hot-reload + dialog teardown is only
    // correct for a genuine EXTERNAL hand-edit. If the bytes on disk are
    // identical to what we last wrote, this event is our own echo — skip
    // the reload, the cached-dialog teardown, and the "Config reloaded"
    // toast, so the open Settings dialog survives an Apply / tab-switch
    // (ANTS-1981). A real external edit differs in bytes and falls through.
    {
        QFile cf(path);
        if (cf.open(QIODevice::ReadOnly)) {
            const QByteArray onDisk = cf.readAll();
            if (!onDisk.isEmpty() && onDisk == m_config.lastWrittenBytes()) {
                QTimer::singleShot(0, this, [this]() { m_inConfigReload = false; });
                return;
            }
        }
    }

    // Reload config from disk
    m_config = Config();

    // ANTS-2085 — re-publish the terse-by-default preference after an
    // external config edit (the Settings dialog's own Apply re-publishes
    // directly, since a self-write echo short-circuits above).
    mcp::setTerseDefault(m_config.claudeMcpTerseResponses());
    // ANTS-2094 — re-publish result-offload config after an external edit.
    mcp::setOffloadConfig(m_config.claudeMcpOffloadLargeResults(),
                          m_config.claudeMcpOffloadThresholdBytes(),
                          m_config.claudeMcpOffloadHeadBytes());

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

    // Re-apply all settings. applyTheme is skipped when the value didn't
    // change because applyTheme rewrites the QSS, restyles every widget,
    // and (via setTheme) used to write the config back — which is what
    // started the inotify loop in the first place. The setter is now
    // idempotent, but skipping the whole applyTheme call is also cheaper
    // when the reload is just a window-geometry tick or similar.
    const QString newTheme = m_config.theme();
    if (newTheme != m_currentTheme) applyTheme(newTheme);
    applyFontSizeToAll(m_config.fontSize());

    QList<TerminalWidget *> terminals = liveTerminals();
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

    // Clear the re-entrancy flag on the next event-loop tick rather than
    // immediately. Any inotify event queued by a save() inside this slot
    // is read by Qt as soon as control returns to the loop; deferring the
    // clear by 0 ms (singleShot) ensures we drop *that* re-entry, not the
    // user's next genuine external edit.
    QTimer::singleShot(0, this, [this]() { m_inConfigReload = false; });
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
    // ANTS-1138 — clear caches when auto_profile_rules has been
    // edited since last call. Otherwise retired patterns linger
    // forever in s_patternCache (small leak; ~few KB per
    // orphaned regex over a power-user session that edits rules
    // many times) and `s_warnedInvalid` never re-warns when a
    // fixed-then-rebroken pattern gets edited a third time.
    static quint64 s_lastRulesGen = 0;
    const quint64 currentGen = m_config.autoProfileRulesGeneration();
    if (currentGen != s_lastRulesGen) {
        s_patternCache.clear();
        s_warnedInvalid.clear();
        s_lastRulesGen = currentGen;
    }

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
    // ANTS-1174: replace the proxy holder so previous proxies are
    // destroyed before fresh ones are built — prevents N-rebuild
    // accumulation under plugin reloads / config refreshes.
    delete m_paletteProxyHolder;
    m_paletteProxyHolder = new QObject(this);
    QList<QAction *> all;
    for (QAction *menuAction : m_menuBar->actions()) {
        if (menuAction->menu())
            collectActions(menuAction->menu(), m_paletteProxyHolder, all);
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
