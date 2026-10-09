// ANTS-1677 mainwindow piece 4/8 — session persistence
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "claudestatuswidgets.h"
#include "coloredtabbar.h"
#include "terminalwidget.h"
#include "sessionmanager.h"
#include "claudeintegration.h"
#include "claudetabtracker.h"
#include <QApplication>
#include <QDir>
#include <QSplitter>
#include <QTimer>
#include <algorithm>

using namespace mainwindowdetail;

// --- Session persistence ---

void MainWindow::saveAllSessions(bool force) {
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

        // ANTS-5030 — skip a tab whose blob would be byte-identical to the
        // one already on disk. serialize() streams every cell of the grid,
        // scrollback included, then compresses, hashes, writes and fsyncs
        // it, all on the GUI thread; at the 50k default that is tens of MB
        // per tab every 30 s for a tab nobody has touched.
        const TerminalGrid *g = t->grid();
        SessionSaveKey key;
        key.revision = g->contentRevision();
        key.scrollbackPushed = g->scrollbackPushed();
        key.rows = g->rows();
        key.cols = g->cols();
        key.cursorRow = g->cursorRow();
        key.cursorCol = g->cursorCol();
        key.title = g->windowTitle();
        key.cwd = t->shellCwd();
        key.pinnedTitle = pinnedTitle;

        // ANTS-5131 — a tab whose last async save compressed past its cap
        // wrote nothing; redo it here, where the grid can be re-read.
        const bool overshot = SessionManager::takeOvershoot(tabId);
        const auto seen = m_sessionSaveKeys.constFind(tabId);
        if (!force && !overshot && seen != m_sessionSaveKeys.cend() && *seen == key)
            continue;

        // ANTS-5131 — the compress, hash and write go to a worker; the
        // forced save at close stays synchronous.
        if (force || overshot)
            SessionManager::saveSession(tabId, g, key.cwd, pinnedTitle);
        else
            SessionManager::saveSessionAsync(tabId, g, key.cwd, pinnedTitle);
        m_sessionSaveKeys.insert(tabId, key);
    }
    // Drop keys for tabs this window no longer holds, so the cache cannot
    // outgrow the tab set over a long-lived session.
    if (m_sessionSaveKeys.size() > tabOrder.size()) {
        const QSet<QString> live(tabOrder.cbegin(), tabOrder.cend());
        for (auto it = m_sessionSaveKeys.begin(); it != m_sessionSaveKeys.end();) {
            if (live.contains(it.key()))
                ++it;
            else
                it = m_sessionSaveKeys.erase(it);
        }
    }
    saveProcessTabOrder(tabOrder, activeIndex);
}

// ANTS-1159 — cheap tab-order-only save. Builds this window's tab
// order + active index and writes it through saveProcessTabOrder.
// Does NOT touch the per-tab
// scrollback `.dat` files (saveAllSessions handles those, on
// the 30 s timer + closeEvent). Mirrors saveAllSessions's
// guards — sessionPersistence + 5 s uptime floor.
void MainWindow::saveTabOrderOnly() {
    if (!m_config.sessionPersistence()) return;
    if (m_uptimeTimer.elapsed() < 5000) return;

    int activeIndex = 0;
    const QStringList tabOrder = sessionTabIds(&activeIndex);
    saveProcessTabOrder(tabOrder, activeIndex);
}

// ANTS-5032 — this window's tabs as session ids, in tab-bar order.
// *activeIndex, when given, becomes the current tab's position in it.
QStringList MainWindow::sessionTabIds(int *activeIndex) const {
    QStringList ids;
    const int currentIdx = m_tabWidget->currentIndex();

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        auto *t = activeTerminalInTab(w);
        if (!t) continue;

        QString tabId = m_tabSessionIds.value(w);
        if (tabId.isEmpty() && t != w)
            tabId = m_tabSessionIds.value(t);
        if (tabId.isEmpty()) continue;

        if (activeIndex && i == currentIdx)
            *activeIndex = ids.size();
        ids.append(tabId);
    }
    return ids;
}

// ANTS-5032 — tab_order.txt is one file per process, so each window
// writes every window's tabs, its own first so activeIndex still points
// at its current tab. Hidden windows count: a Quake window hides but
// keeps its tabs. Restore reopens the whole list in the first window.
void MainWindow::saveProcessTabOrder(QStringList tabOrder, int activeIndex) const {
    for (QWidget *top : QApplication::topLevelWidgets()) {
        const auto *other = qobject_cast<const MainWindow *>(top);
        if (other && other != this)
            tabOrder += other->sessionTabIds();
    }
    SessionManager::saveTabOrder(tabOrder, activeIndex);
}

// ANTS-5118 — whether closing this window leaves another on screen.
// Visible only: Qt quits once no visible window remains, so a hidden
// Quake window keeps neither the app nor this window's tabs alive.
bool MainWindow::anotherWindowStaysOpen() const {
    for (QWidget *top : QApplication::topLevelWidgets()) {
        const auto *other = qobject_cast<const MainWindow *>(top);
        if (other && other != this && other->isVisible())
            return true;
    }
    return false;
}

void MainWindow::restoreSessions() {
    // ANTS-5032 — once per process. A later window (File → New Window)
    // keeps the fresh tab newTab gave it instead of reopening the saved
    // tabs under ids the first window already owns.
    static bool restored = false;
    if (restored) return;
    restored = true;
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
