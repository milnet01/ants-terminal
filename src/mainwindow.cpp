#include "mainwindow.h"
#include "mainwindow_internal.h"

#include "tokenusageengine.h"  // ANTS-3572 — foldMonthlyBucket / sumYear
#include "guithread.h"       // ANTS-4682 — ants::onGuiThread in inline verbs
#include <QDate>
#include <QJsonObject>
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
#include "localsockethub.h"
#include "resolvedroot.h"      // ANTS-1401 — terminalForCaller helper
#include "rootprovider.h"      // ANTS-4932 § 2.4
#include "mcptoolregistry.h"   // ANTS-4932 § 2.3
#include "mcpdsocket.h"         // ANTS-5236 — reapStaleTerminalSockets
#include "secureio.h"          // ANTS-4456 — ensurePrivateDir (0700)
#include "reviewbuttonstate.h" // ANTS-1874 — Review-button porcelain predicate
#include "gitwrap.h"           // ANTS-4999 — readOnlyEnvironment for git probes
#include "hostexec.h"          // ANTS-5598 — git and gh run on the host inside a Flatpak
#include "verifytrustmodal.h"  // ANTS-1337 Phase 2
#include "verifytrustprompt.h" // ANTS-5464
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
#include "claudesetup.h"        // ANTS-5236 — refreshStatusHookScript
#include "claudestatuswidgets.h"
#include "claudetabtracker.h"
#include "mcpprojection.h"   // ANTS-2085 — mcp::setTerseDefault
#include "mcpspill.h"        // ANTS-2094 — mcp::setOffloadConfig / spillSweep
#include "mcporientation.h"  // ANTS-1897 — SessionStart hook installer.
#include "themedstylesheet.h"
#include "claudeprojects.h"
#include "claudetranscript.h"
#include "aboutdialogs.h"          // ANTS-1181 — About-Ants/About-Qt
#include "welcomedialog.h"         // ANTS-5558 — first-run welcome
#include "selfupdate.h"           // ANTS-5560 — the self-updater
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

using namespace mainwindowdetail;

namespace mainwindowdetail {
// Forward declaration — definition lives next to setupQuakeMode() (its
// only other caller) so the conversion table is one scroll away from
// the portal binding.
QString qtKeySequenceToPortalTrigger(const QString &qtHotkey);

// Defined below, after the Qt includes — sweeps stale
// `/tmp/kwin_*_ants_*.js` orphans on startup.
void sweepKwinScriptOrphansOnce();
}  // namespace mainwindowdetail

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
#include <QColorDialog>  // ANTS-1374 — custom per-tab colour picker
#include <QTableWidget>
#include <QHeaderView>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QRegularExpression>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QScopeGuard>
#include <QSystemTrayIcon>
#include <QWindow>

// ANTS-1323: configure-time build-date / build-time macros for the
// window-title build-badge.
#include "build_info.h"

#ifdef ANTS_WAYLAND_LAYER_SHELL
#include <LayerShellQt/Window>
#endif

namespace mainwindowdetail {
// ANTS-5560 — set by Restart now, cleared if a window refuses to close; read
// once the quit is committed. Process-wide because Restart now closes every
// window.
bool g_relaunchOnQuit = false;
}  // namespace mainwindowdetail

namespace mainwindowdetail {
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


// ANTS-1357: the literal lives at ClaudeIntegration::kMcpRcUnavailable
// — shared so the idempotent-read cache can reject the same bytes
// at insert time (INV-5(b)). Re-aliased here for local readability.
const char *const kRcUnavailable = ClaudeIntegration::kMcpRcUnavailable;

}  // namespace mainwindowdetail

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
    // ANTS-4273 — PluginManager re-emits the engine's showNotification, and
    // nothing consumed it, so ants.notify() accepted its arguments and did
    // nothing. PLUGINS.md promises a desktop notification "or falls back to
    // the status bar", so the status bar is used when delivery fails rather
    // than as well as it.
    connect(m_pluginManager, &PluginManager::showNotification, this,
            [this](const QString &title, const QString &message) {
        if (!showDesktopNotification(title, message)) {
            showStatusMessage(title.isEmpty() ? message
                                              : title + QStringLiteral(" — ") + message,
                              5000);
        }
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
            [this](const QString &pluginName, const QString &key, const QString &value,
                   QString &error) {
                error = m_config.setPluginSetting(pluginName, key, value);
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
            this, [this] { saveAllSessions(); });
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

    // ANTS-5223 — red chip while broadcast input is on, so a mode that copies
    // typing into other panes is never invisible. refreshBroadcastChip()
    // styles and shows it.
    m_broadcastChip = new QLabel(tr("Broadcast"), this);
    m_broadcastChip->setObjectName(QStringLiteral("broadcastChip"));
    m_broadcastChip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    m_broadcastChip->setAccessibleName(tr("Broadcast input is on"));
    m_broadcastChip->setToolTip(
        tr("Typing is copied to every pane in this tab. "
           "Turn it off under Settings → Broadcast Input."));
    m_broadcastChip->hide();
    statusBar()->addWidget(m_broadcastChip);

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
    // ANTS-5620 — unread-mail chip; re-queries only when the cwd or the
    // store's files changed, so the tick is a few stat() calls otherwise.
    connect(m_statusTimer, &QTimer::timeout, m_claudeStatusBarController,
            &ClaudeStatusBarController::refreshMailChip);
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
    // and answers the commands RemoteControl::dispatch handles
    // (src/remotecontrol_terminal.cpp). Failure to bind (another Ants instance
    // already owns the socket) is non-fatal: the log notes it and
    // the main window boots normally.
    //
    // ANTS-5090 — keep this AFTER the status-bar chrome setup above, which
    // creates m_claudeIntegration. Qt deletes children in creation order, so
    // ~ClaudeIntegration joins the dispatch worker before RemoteControl is
    // destroyed; an off-thread verb running a cmd*() at teardown still finds
    // it alive. Locked by mcp_verb_offthread_guard INV-7.
    m_remoteControl = new RemoteControl(this, this, m_rootProvider.get());
    // ANTS-5144 — the listener is shared by every window and prefers a
    // visible one when it picks who serves a request.
    m_remoteControl->setWindowVisibleProbe([this] { return isVisible(); });
    // ANTS-3661 § 2.4 / ANTS-3688 — inject the verb vocabulary doc_symbols
    // excludes from its candidate harvest. Here rather than in the MCP
    // provider-setup function, because that runs from setupStatusBarChrome
    // far earlier in this constructor, when m_remoteControl is still null: the
    // `if (m_remoteControl)` guard there failed silently and the provider was
    // never installed, so excludedNames held only the refusal codes and every
    // MCP verb name became an unresolved_symbol finding. The tool registrations
    // in that function survive the same ordering because rcDelegate derefs
    // m_remoteControl lazily at call time; this setter does not, which is why
    // it alone had to move. Read lazily on each call, so a verb registered
    // after this line is still covered.
    m_remoteControl->setMcpVerbVocabularyProvider([this] {
        return m_claudeIntegration ? m_claudeIntegration->registeredToolNames()
                                   : QStringList();
    });
    // ANTS-5506 — doc_facts' argument map, installed beside the vocabulary
    // provider for the same ordering reason. It returns the copy tools/list
    // publishes, never m_toolParamKeys (cmdDocLint runs on a worker).
    m_remoteControl->setMcpVerbArgsProvider([this] {
        return m_claudeIntegration ? m_claudeIntegration->verbArgsSnapshot()
                                   : QHash<QString, QSet<QString>>();
    });
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
    // ANTS-5464 — ants-mcpd has no window, so it asks here; the same client
    // decides, from this process's own read of the config. Runs on the Bulk
    // worker, beside verify_changes, which uses the same client.
    if (m_claudeIntegration) {
        m_claudeIntegration->setVerifyTrustPromptHandler(
            [this](const QJsonObject &params) {
                return VerifyTrust::answerPromptRequest(
                    params, m_remoteControl ? m_remoteControl->verifyTrustClient()
                                            : nullptr);
            });
    }
    // ANTS-2132 § 2.7 — the socket's routes with an off-thread MCP twin run on
    // the MCP dispatch worker, so a --remote search no longer freezes the
    // window and RemoteControl's roadmap store stays on one thread (ANTS-5073,
    // ANTS-5051). Installed before start(), so no request is served inline
    // first.
    if (m_claudeIntegration) {
        m_remoteControl->setDispatchWorkerPoster(
            [ci = m_claudeIntegration](std::function<void()> job) {
                return ci->postWorkerJob(std::move(job));
            });
    }
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
    // ANTS-5036 — m_config dies with this window; dialogs opened from
    // another window must not persist their size through it.
    DialogChrome::releaseConfig(&m_config);

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

// ANTS-5558 — one welcome dialog at a time: a second request raises it.
void MainWindow::showWelcome() {
    if (m_welcomeDialog) {
        m_welcomeDialog->raise();
        m_welcomeDialog->activateWindow();
        return;
    }
    auto *dlg = new WelcomeDialog(QString(), WelcomeDialog::detect(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_welcomeDialog = dlg;
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::refreshBroadcastChip() {
    if (!m_broadcastChip) return;
    const Theme &th = Themes::byName(m_currentTheme);
    m_broadcastChip->setStyleSheet(
        themedstylesheet::buildChipStylesheet(th, th.ansi[1], /*leftMarginPx=*/4));
    m_broadcastChip->setVisible(m_broadcastMode);
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
    refreshBroadcastChip();

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

    runKWinScript(kwinJs, QStringLiteral("move"));
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

    runKWinScript(kwinJs, QStringLiteral("center"));
}

// ANTS-2205 (7b) — shared KWin scripting chain extracted from moveViaKWin /
// centerWindow (was ~35 duplicated lines each). Writes the script body to an
// unpredictable tempfile (0.7.12 TOCTOU fix; see kwinpositiontracker.cpp), then
// runs the async loadScript → start → unloadScript chain and removes the temp
// file. The `tag` distinguishes the two callers in both the tempfile prefix
// (kwin_<tag>_ants_*) and the registered script name (ants_terminal_<tag>).
// Callers guard on kwinPresent() before invoking.
void MainWindow::runKWinScript(const QString &kwinJs, const QString &tag) {
    const QString scriptName = QStringLiteral("ants_terminal_%1").arg(tag);
    QString scriptPath;
    {
        QTemporaryFile f(QDir::tempPath() +
                         QStringLiteral("/kwin_%1_ants_XXXXXX.js").arg(tag));
        f.setAutoRemove(false);
        if (!f.open()) return;
        f.write(kwinJs.toUtf8());
        scriptPath = f.fileName();
    }

    // Run KWin script asynchronously to avoid blocking the event loop.
    auto *proc = new QProcess(this);
    // ANTS-5079 — a dbus-send that fails to start never emits finished, so
    // without this the process object and the temp script leaked.
    connect(proc, &QProcess::errorOccurred, this,
            [proc, scriptPath](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        proc->deleteLater();
        QFile::remove(scriptPath);
    });
    proc->start("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QStringLiteral("string:%1").arg(scriptPath),
        QStringLiteral("string:%1").arg(scriptName)
    });
    connect(proc, &QProcess::finished, this, [this, proc, scriptPath, scriptName]() {
        proc->deleteLater();
        auto *proc2 = new QProcess(this);
        connect(proc2, &QProcess::errorOccurred, this,
                [proc2, scriptPath](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;  // ANTS-5079
            proc2->deleteLater();
            QFile::remove(scriptPath);
        });
        proc2->start("dbus-send", {
            "--session", "--dest=org.kde.KWin", "--print-reply",
            "/Scripting", "org.kde.kwin.Scripting.start"
        });
        connect(proc2, &QProcess::finished, this, [proc2, scriptPath, scriptName]() {
            proc2->deleteLater();
            QProcess::startDetached("dbus-send", {
                "--session", "--dest=org.kde.KWin", "--print-reply",
                "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                QStringLiteral("string:%1").arg(scriptName)
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
        // ANTS-5558 — once per config, after the window has settled.
        QTimer::singleShot(600, this, [this]() {
            welcome::maybeAutoShow(m_config, [this] { showWelcome(); });
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
    m_claudeIntegration->setContextWindowTokens(m_config.claudeContextWindowTokens());
    m_claudeTabTracker  = new ClaudeTabTracker(this);
    // ANTS-5144 — the MCP and hook listeners are shared by every window. They
    // prefer a visible window, and a hook event goes to the window whose tabs
    // track its session.
    m_claudeIntegration->setWindowVisibleProbe([this] { return isVisible(); });
    m_claudeIntegration->setSessionOwnerProbe([this](const QString &sessionId) {
        return m_claudeTabTracker &&
               m_claudeTabTracker->shellForSessionId(sessionId) > 0;
    });

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

    // ANTS-3572 — fold the session's tokens-saved total into the persisted
    // aggregate just before the engine resets. The default (Auto → Direct,
    // same thread) connection runs the slot synchronously inside emit, so the
    // fold reads the intact total before reset() clears it (INV-3).
    connect(m_claudeIntegration, &ClaudeIntegration::tokenSessionEnding,
            this, &MainWindow::foldTokenSavingsIntoConfig);

    // ANTS-5311 — ants-mcpd serves most calls and writes its counts to a
    // snapshot here; the terminal's own tokensSavedUpdated never fires for
    // them. A helper's writes and its final one are renames in this
    // directory, so watching it both refreshes the chip and folds a helper's
    // snapshot moments after it exits. Created first: a watcher on a missing
    // directory never fires.
    {
        const QString peerDir = TokenUsageEngine::peerSnapshotDir();
        // ensurePrivateDir creates it 0700; mkpath + chmod left it at the
        // umask's mode until the chmod landed (ANTS-5311 spec: "mode 0700").
        if (ensurePrivateDir(peerDir)) {
            m_peerUsageWatcher = new QFileSystemWatcher({peerDir}, this);
            connect(m_peerUsageWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
                foldDeadPeers();
                if (m_claudeStatusBarController)
                    m_claudeStatusBarController->refreshTokensSavedChip();
            });
        }
        foldDeadPeers();   // snapshots left while no terminal was running
    }

    setupClaudeMcpProviders();

    // 0.7.39 — Roadmap button. Sibling to Background Tasks; same size/
    // policy contract. Hidden until the active tab's cwd is probed and
    // a ROADMAP.md surfaces. User asked for it to follow the
    // ROADMAP.md-presence convention so terminals running outside any
    // project root pay nothing.
    m_roadmapBtn = new QPushButton(tr("Roadmap"), this);
    // ANTS-2049 — objectName hook for the e2e harness (inject-click by
    // objectName to open the Roadmap dialog; smoke case 3).
    m_roadmapBtn->setObjectName(QStringLiteral("roadmapButton"));
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
    // ANTS-5560 — update.check_on_startup (Settings → General) gates this
    // startup check only; Help → Check for Updates always runs.
    QTimer::singleShot(5000, this, [this]() {
        if (m_config.updateCheckOnStartup()) checkForUpdates(/*userInitiated=*/false);
    });
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
            // ANTS-5080 — the session id is quoted like the path.
            QString cmd = QString("cd %1 && claude --resume %2")
                          .arg(shellQuote(projectPath), shellQuote(sessionId));
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
            // ANTS-5144 § 2.3 — the shared listeners serve the most recently
            // activated window.
            ants::LocalSocketHub::instance().noteActivated(m_claudeIntegration);
            ants::LocalSocketHub::instance().noteActivated(m_remoteControl);
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
    // ANTS-5120 — ask first, as closing one tab does, when a program other
    // than a shell runs in any pane of this window. The dialog is
    // non-modal, so this close is refused and Close anyway closes again.
    if (!m_closeConfirmed && m_config.confirmCloseWithProcesses()) {
        for (TerminalWidget *t : liveTerminals()) {
            const QString running =
                t->shellPid() > 0 ? firstNonShellDescendant(t->shellPid()) : QString();
            if (!running.isEmpty()) {
                event->ignore();
                showCloseWindowConfirmDialog(running);
                return;
            }
        }
    }
    m_closeConfirmed = false;

    // ANTS-1159 — stop the periodic session-save timer first so
    // a tick landing mid-shutdown can't re-enter the save path
    // and race the explicit shutdown save below.
    if (m_sessionSaveTimer) m_sessionSaveTimer->stop();

    // ANTS-5118 — closing a window while another visible one stays open is
    // final for its tabs: close them, which ends their shells. The first
    // window only hides on close, so without this its programs ran on
    // unseen. Deferred one event-loop turn, when a New Window window's deleteLater ends its tabs, and
    // re-checked then: closes that arrive together (a Plasma logout closes
    // every window) leave the last window to save every tab first.
    if (anotherWindowStaysOpen()) {
        QTimer::singleShot(0, this, [this] {
            if (isVisible() || !anotherWindowStaysOpen()) return;
            while (m_tabWidget->count() > 0)
                performTabClose(m_tabWidget->count() - 1);
        });
    }

    QPoint realPos = m_posTracker->currentPos();
    m_config.setWindowGeometry(realPos.x(), realPos.y(), width(), height());
    // Persist tab color sequence unconditionally — independent of
    // session persistence, so the fallback restore path can apply
    // colors at next launch even when scrollback isn't saved.
    saveTabColorSequence();
    saveAllSessions(/*force=*/true);
    // ANTS-3572 — fold the final MCP session's savings before we exit; the
    // case the initialize-time fold can't cover (a new process starts empty).
    // endTokenSession folds (via tokenSessionEnding) then resets.
    if (m_claudeIntegration) m_claudeIntegration->endTokenSession();
    event->accept();
}

// ANTS-3572 — persist the just-ended session's tokens-saved total. Triggered
// synchronously by ClaudeIntegration::tokenSessionEnding (before the counter
// resets) and at app quit. One config write after the store-only setters
// (INV-7); idempotent — a 0-total (repeat) call is a no-op (INV-2).
void MainWindow::foldTokenSavingsIntoConfig() {
    if (!m_claudeIntegration) return;
    const QString month = QDate::currentDate().toString("yyyy-MM");
    bool dirty = false;
    // ANTS-3572 — global fold (unchanged): runs FIRST, only when the session
    // saved something globally (INV-4).
    const qint64 s =
        m_claudeIntegration->tokenUsageReport(/*includeZero=*/false).totalSaved;
    if (s > 0) {
        m_config.setClaudeTokensSavedMonthly(TokenUsageEngine::foldMonthlyBucket(
            m_config.claudeTokensSavedMonthly(), month, s, /*keepMonths=*/24));
        m_config.setClaudeTokensSavedLifetime(
            m_config.claudeTokensSavedLifetime() + s);
        if (m_config.claudeTokensSavedSince().isEmpty())
            m_config.setClaudeTokensSavedSince(
                QDate::currentDate().toString(Qt::ISODate));
        dirty = true;
    }
    // ANTS-3579 — per-project fold: snapshot the live bytes map, convert to
    // tokens, fold each root (threading the accumulator), then ONE prune pass
    // (M-2). Independent of the global aggregate; shares the single save below.
    const QHash<QString, qint64> byProj =
        m_claudeIntegration->sessionSavedBytesByProject();
    if (!byProj.isEmpty()) {
        QJsonObject bp = m_config.claudeTokensSavedByProject();
        const QString nowIso = QDateTime::currentDateTime().toString(Qt::ISODate);
        bool folded = false;
        for (auto it = byProj.constBegin(); it != byProj.constEnd(); ++it) {
            const qint64 tokens = it.value() / TokenUsageEngine::kCharsPerToken;
            if (tokens <= 0) continue;
            bp = TokenUsageEngine::foldProjectBucket(
                bp, it.key(), tokens, month, nowIso, /*keepMonths=*/24);
            folded = true;
        }
        if (folded) {
            bp = TokenUsageEngine::pruneProjectBuckets(bp, /*keepProjects=*/64);
            m_config.setClaudeTokensSavedByProject(bp);
            dirty = true;
        }
    }
    // ANTS-5311 — exited ants-mcpd helpers' snapshots, in the same single
    // write (their INV-9); the files go only after the save.
    const QString peerDir = TokenUsageEngine::peerSnapshotDir();
    TokenUsageEngine::PeerUsage dead;
    QList<int> locks;
    TokenUsageEngine::readPeerSnapshots(peerDir, true, &dead, &locks);
    if (foldClaimedPeersIntoConfig(dead)) dirty = true;
    if (dirty) m_config.save();  // single write, global + per-project (INV-6)
    TokenUsageEngine::releaseClaimed(peerDir, dead, locks);
}

void MainWindow::foldDeadPeers() {
    const QString peerDir = TokenUsageEngine::peerSnapshotDir();
    TokenUsageEngine::PeerUsage dead;
    QList<int> locks;
    TokenUsageEngine::readPeerSnapshots(peerDir, true, &dead, &locks);
    if (foldClaimedPeersIntoConfig(dead)) m_config.save();
    TokenUsageEngine::releaseClaimed(peerDir, dead, locks);
}

bool MainWindow::foldClaimedPeersIntoConfig(const TokenUsageEngine::PeerUsage &dead) {
    if (dead.sessions == 0) return false;
    QJsonObject monthly = m_config.claudeTokensSavedMonthly();
    qint64 lifetime = m_config.claudeTokensSavedLifetime();
    QJsonObject byProject = m_config.claudeTokensSavedByProject();
    if (!TokenUsageEngine::foldPeerUsage(
            dead, monthly, lifetime, byProject,
            QDate::currentDate().toString(QStringLiteral("yyyy-MM")),
            QDateTime::currentDateTime().toString(Qt::ISODate)))
        return false;
    m_config.setClaudeTokensSavedMonthly(monthly);
    m_config.setClaudeTokensSavedLifetime(lifetime);
    m_config.setClaudeTokensSavedByProject(byProject);
    if (m_config.claudeTokensSavedSince().isEmpty())
        m_config.setClaudeTokensSavedSince(QDate::currentDate().toString(Qt::ISODate));
    return true;
}

// ANTS-3572 — assemble the tokens-saved summary for the token_usage MCP verb.
// Each period = stored + live session; monthly[] is the folded buckets only,
// recent-first. Reads the same single ClaudeIntegration the verb's `ci` points
// at, so verb numbers are mutually consistent (INV-1).
TokenSavingsSummary MainWindow::tokenSavingsSummary(
        const TokenUsageEngine::PeerUsage &peers) const {
    TokenSavingsSummary out;
    // ANTS-5311 — the live session is the terminal's own plus every ants-mcpd
    // snapshot not yet folded, each derived per snapshot.
    const qint64 session = (m_claudeIntegration
        ? m_claudeIntegration->tokenUsageReport(false).totalSaved : 0) + peers.savedTokens;
    const QJsonObject monthly = m_config.claudeTokensSavedMonthly();
    const QString curMonth = QDate::currentDate().toString("yyyy-MM");
    const QString curYear  = QDate::currentDate().toString("yyyy");
    out.month    = static_cast<qint64>(monthly.value(curMonth).toDouble(0)) + session;
    out.ytd      = TokenUsageEngine::sumYear(monthly, curYear) + session;
    out.lifetime = m_config.claudeTokensSavedLifetime() + session;
    // Recent-first: QJsonObject::keys() is ascending, so reverse-iterate.
    const QStringList keys = monthly.keys();
    for (auto it = keys.crbegin(); it != keys.crend(); ++it) {
        QJsonObject e;
        e["month"] = *it;
        e["saved"] = monthly.value(*it).toDouble(0);
        out.monthly.append(e);
    }
    return out;
}

// --- Status bar ---

// ANTS-4273 — extracted from the OSC 9/777 lambda when ants.notify() became a
// second caller, rather than copying it. The focus gate is deliberately NOT in
// here: the terminal path suppresses a notification while the window is
// focused, and an explicit ants.notify() call from a plugin is an intentional
// act that PLUGINS.md documents no gate for.
bool MainWindow::showDesktopNotification(const QString &title, const QString &body) {
    const QString shown = title.isEmpty() ? QStringLiteral("Ants Terminal") : title;
    auto *tray = QSystemTrayIcon::isSystemTrayAvailable()
        ? findChild<QSystemTrayIcon *>() : nullptr;
    if (tray) {
        tray->showMessage(shown, body);
        return true;
    }
    return QProcess::startDetached(QStringLiteral("notify-send"), {shown, body});
}

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
        if (m_claudeStatusBarController) {  // ANTS-3579 — no tab → hide the pill
            m_claudeStatusBarController->refreshTokensSavedChip();
            m_claudeStatusBarController->refreshMailChip();   // ANTS-5620
        }
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
    // ANTS-3579 (INV-7) — re-scope the tokens-saved pill to the new tab's
    // project, so a tab switch shows that project's savings with no MCP call.
    if (m_claudeStatusBarController) {
        m_claudeStatusBarController->refreshTokensSavedChip();
        // ANTS-5620 — and the unread-mail chip for the new tab's project.
        m_claudeStatusBarController->refreshMailChip();
    }

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
        // Inject text into the PTY whose output matched — never the focused
        // one, which may be another tab (a Claude session) when the match
        // came from a background tab. \n / \r in the action value pass
        // through verbatim so a "yes\n" rule can auto-answer a prompt —
        // caller's responsibility to scope this with a tight regex.
        if (auto *t = qobject_cast<TerminalWidget *>(sender())) {
            t->sendToPty(actionValue.toUtf8());
        }
    }
    showStatusMessage(QString("Trigger: '%1' matched").arg(pattern), 3000);
}

// --- Diff Viewer ---

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
    if (m_reviewProbeInFlight) {
        // ANTS-5080 — the running probe is for another cwd, and its result
        // will not be shown here, so do not leave that tab's state on screen.
        if (m_reviewProbeCwd != cwd) reviewBtn->hide();
        return;
    }

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
    // ANTS-4999 — a plain status may take .git/index.lock to refresh the
    // index, and on a 2 s timer that makes the user's git commit fail.
    proc->setProcessEnvironment(GitWrap::readOnlyEnvironment());
    proc->setProgram("git");
    proc->setArguments({"status", "--porcelain=v1", "-b"});

    QPointer<QPushButton> btn = reviewBtn;
    QPointer<QProcess> guard = proc;
    QPointer<MainWindow> self(this);
    m_reviewProbeInFlight = true;
    m_reviewProbeCwd = cwd;
    connect(proc, &QProcess::finished, this,
            [btn, guard, self, cwd](int exitCode, QProcess::ExitStatus status) {
        // ANTS-5080 — apply the result only to the tab it was probed for.
        bool stillCurrent = false;
        if (auto *p = self.data()) {
            TerminalWidget *now = p->focusedTerminal();
            if (!now) now = p->currentTerminal();
            stillCurrent = now && now->shellCwd() == cwd;
        }
        if (btn && stillCurrent) {
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
    HostExec::start(*proc);
    // ANTS-5080 — a git status that never exits must not hold the flag for
    // the rest of the session. kill() delivers finished, which clears it.
    QTimer::singleShot(10000, proc, [guard]() {
        if (guard && guard->state() != QProcess::NotRunning) guard->kill();
    });
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

void MainWindow::restartForUpdate() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        connect(qApp, &QCoreApplication::aboutToQuit, qApp, [] {
            if (!g_relaunchOnQuit) return;
            const SelfUpdate::Relaunch r = SelfUpdate::relaunchCommand(
                QCoreApplication::applicationPid(), qEnvironmentVariable("APPIMAGE"),
                QProcessEnvironment::systemEnvironment());
            QProcess p;
            p.setProgram(r.program);
            p.setArguments(r.arguments);
            p.setProcessEnvironment(r.environment);
            p.startDetached();
        });
    }
    g_relaunchOnQuit = true;
    // The normal close path, so each window saves its session. A window may
    // still ask about running programs; its Cancel clears the flag.
    QApplication::closeAllWindows();
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
    // ANTS-3550 — re-publish the advisory-hint latch after an external edit
    // (live off-switch: flip claude.mcp_hint_latch to get the tips back).
    mcp::setHintLatchEnabled(m_config.claudeMcpHintLatch());
    // ANTS-2094 — re-publish result-offload config after an external edit.
    mcp::setOffloadConfig(m_config.claudeMcpOffloadLargeResults(),
                          m_config.claudeMcpOffloadThresholdBytes(),
                          m_config.claudeMcpOffloadHeadBytes());
    // The context meter's window, after a hand edit of config.json.
    if (m_claudeIntegration)
        m_claudeIntegration->setContextWindowTokens(
            m_config.claudeContextWindowTokens());

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
