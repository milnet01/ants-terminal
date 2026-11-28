// ANTS-1677 mainwindow piece 4/9 — session persistence
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

        const QString tabId = m_tabSessionIds.value(w);
        if (tabId.isEmpty()) continue;

        // Track active index as position within tabOrder, not the tab widget
        if (i == currentIdx)
            activeIndex = tabOrder.size();

        tabOrder.append(tabId);
        // ANTS-5225 — a sleeping tab's file is its only copy of its
        // history; its empty grid must not overwrite it.
        if (m_sleepingTabs.contains(t)) continue;
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

void MainWindow::saveSessionsForShutdown() {
    if (m_sessionSaveTimer) m_sessionSaveTimer->stop();
    saveTabColorSequence();
    saveAllSessions(/*force=*/true);
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

        const QString tabId = m_tabSessionIds.value(w);
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
    // ANTS-5654 — every stray file only when no tab list was ever saved. A
    // list naming only lost tabs reopened tabs closed weeks before.
    if (sessions.isEmpty() && !SessionManager::hasTabOrder())
        sessions = SessionManager::savedSessions();
    if (sessions.isEmpty()) return;

    // Close the default empty tab that was created at startup
    bool hadDefaultTab = (m_tabWidget->count() == 1);

    // ANTS-5225 — startup reads only each tab's title, cwd and pin. Its
    // history loads and its shell starts when the tab is first needed
    // (wakeRestoredTab). A tab whose peek is refused is woken below anyway,
    // so its loadSession moves the file aside as before.
    QList<TerminalWidget *> restoredTabs;
    // Guarded: a tab can close (rc close-tab, the tab's x) before the
    // deferred wake below runs, and the widget is then deleted.
    QList<QPointer<TerminalWidget>> wakeNow;
    QHash<TerminalWidget *, SleepingTab> sleeping;

    for (const QString &tabId : sessions) {
        auto *terminal = addShellTab(tabId);
        // Re-apply any persisted colour tag for this UUID. Must happen
        // after addTab (tab has an index) and after m_tabSessionIds is
        // populated (so applyPersistedTabColor can resolve the UUID).
        applyPersistedTabColor(terminal);
        restoredTabs.append(terminal);

        SessionPeek saved;
        if (!SessionManager::peekSession(tabId, &saved))
            wakeNow.append(terminal);
        sleeping.insert(terminal, {tabId, saved.cwd});

        // The pin takes precedence over the shell-derived window title —
        // the whole point of the manual rename is that it sticks until the
        // user un-renames.
        const int newTabIdx = m_tabWidget->count() - 1;
        if (!saved.pinnedTitle.isEmpty()) {
            // Re-pin via m_tabTitlePins[terminal] so the titleChanged
            // signal handler and the 2 s updateTabTitles tick both
            // honor it. Written directly rather than through
            // setTabTitleForRemote, which would wake the tab.
            m_tabTitlePins[terminal] = saved.pinnedTitle;
            setTabLabel(newTabIdx, saved.pinnedTitle);
        } else if (!saved.title.isEmpty()) {
            setTabLabel(newTabIdx, saved.title);
        }
    }

    // Remove the default empty tab if we restored sessions
    if (hadDefaultTab && m_tabWidget->count() > 1) {
        QWidget *defaultTab = m_tabWidget->widget(0);
        m_tabSessionIds.remove(defaultTab);
        // ANTS-6636 — newTab() tracked its shell; release it as
        // performTabClose does, or its PID stays in all three trackers.
        if (auto *single = qobject_cast<TerminalWidget *>(defaultTab))
            releaseTerminalShell(single);
        for (auto *pane : defaultTab->findChildren<TerminalWidget *>(QString()))
            releaseTerminalShell(pane);
        m_tabWidget->removeTab(0);
        defaultTab->deleteLater();
    }
    // Only now: the removal above makes the first restored tab current
    // before its geometry is final, and that must not wake it.
    m_sleepingTabs.insert(sleeping);

    updateTabBarVisibility();
    // ANTS-4272 — after the default tab's removal, so the index is final.
    for (TerminalWidget *t : restoredTabs)
        firePluginTabEvent(true, m_tabWidget->indexOf(t), false);

    // Defer tab activation and shell startup until after the event loop has
    // processed layout — widgets need their final geometry so that the grid
    // size computed by startShell matches the actual widget size. Without this,
    // a tab gets a size mismatch that triggers SIGWINCH, causing bash
    // to redraw its prompt (the "double prompt" bug).
    int idx = std::clamp(activeIndex, 0, m_tabWidget->count() - 1);
    QTimer::singleShot(0, this, [this, wakeNow, idx]() {
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

        // onTabChanged wakes the tab it switches to; setCurrentIndex emits
        // nothing when idx is already current, so wake it here too.
        m_tabWidget->setCurrentIndex(idx);
        wakeRestoredTab(terminalAtTab(idx));
        for (TerminalWidget *t : wakeNow)
            wakeRestoredTab(t);
        if (auto *t = focusedTerminal())
            t->setFocus();
    });
}

TerminalWidget *MainWindow::wakeTerminalAtTab(int index) {
    wakeRestoredTab(terminalAtTab(index));
    return terminalAtTab(index);
}

void MainWindow::wakeRestoredTab(TerminalWidget *t) {
    if (!m_sleepingTabs.contains(t)) return;
    // Taken first, so a re-entrant call is a no-op.
    const SleepingTab tab = m_sleepingTabs.take(t);

    // A refused file is moved aside and the tab carries on empty.
    SessionManager::loadSession(tab.tabId, t->grid());
    // Clear screen buffer so new shell starts with a clean display
    // (scrollback history is preserved from the restore above)
    t->grid()->clearScreenContent();
    t->forceRecalcSize();

    QString startDir;
    if (!tab.cwd.isEmpty() && QDir(tab.cwd).exists())
        startDir = tab.cwd;
    if (!t->startShell(startDir, m_config.shellCommand()))
        return;
    t->update();
    // The tab's .dat stays: the next save overwrites it. Deleting it
    // here left no copy until that save, up to 30 s away, and a
    // reboot in that window lost every tab (2026-10-12).
    // ANTS-1375 — without this the per-tab Claude dots stay dark
    // on every tab carried across a restart.
    trackTerminalShell(t);

    // ANTS-5733 — a tab already current gets no tab switch after its shell
    // starts, so nothing else gives ClaudeIntegration this PID; without it
    // the poll stays stopped.
    if (t == focusedTerminal() && m_claudeIntegration && t->shellPid() > 0)
        m_claudeIntegration->setShellPid(t->shellPid());
}
