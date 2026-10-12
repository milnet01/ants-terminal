// ANTS-5645 — the side tab list panel: a list of every open tab by its
// full name, beside the tab widget. Contract:
// tests/features/tab_list_panel/spec.md. Reload: applyTabListPanelConfig()
// runs from both config-reload paths, so no setting needs a relaunch.

#include "mainwindow.h"

#include "coloredtabbar.h"
#include "tablistpanel.h"

#include "commandpalette.h"

#include <QAction>
#include <QPointer>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>

// ANTS-5659 — the one place the tab bar is shown or hidden: one tab needs
// no bar, and the side list can stand in for it. Every site that changes
// the tab count or the panel settings calls this.
void MainWindow::updateTabBarVisibility() {
    if (!m_tabWidget) return;
    const bool listReplacesBar = m_config.tabListPanel() && m_config.tabListPanelHidesTabBar();
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1 && !listReplacesBar);
}

void MainWindow::applyTabListPanelConfig() {
    if (!m_tabSplitter) return;
    const bool show = m_config.tabListPanel();
    if (show && !m_tabListPanel) {
        m_tabListPanel = new TabListPanel(m_tabSplitter);
        m_tabSplitter->insertWidget(0, m_tabListPanel);
        m_tabSplitter->setStretchFactor(0, 0);
        m_tabSplitter->setStretchFactor(1, 1);
        if (m_chromeBg.isValid())
            m_tabListPanel->setThemeColors(m_chromeBg, m_chromeText);
        connect(m_tabListPanel, &TabListPanel::tabActivated, this, [this](QWidget *page) {
            const int i = m_tabWidget->indexOf(page);
            if (i >= 0) m_tabWidget->setCurrentIndex(i);
        });
        connect(m_tabListPanel, &TabListPanel::tabContextMenuRequested, this,
                [this](QWidget *page) {
                    const int i = m_tabWidget->indexOf(page);
                    if (i >= 0) showTabColorMenu(i, true);
                });
        // Same pin as the tab bar's Rename; an empty name clears it.
        connect(m_tabListPanel, &TabListPanel::tabRenamed, this,
                [this](QWidget *page, const QString &name) {
                    const int i = m_tabWidget->indexOf(page);
                    if (i >= 0) setTabTitleForRemote(i, name.trimmed());
                });

        m_tabListRefreshTimer = new QTimer(this);
        m_tabListRefreshTimer->setSingleShot(true);
        m_tabListRefreshTimer->setInterval(50);
        connect(m_tabListRefreshTimer, &QTimer::timeout, this, &MainWindow::refreshTabList);

        // A drag fires splitterMoved per pixel; save once it settles.
        auto *widthSave = new QTimer(this);
        widthSave->setSingleShot(true);
        widthSave->setInterval(500);
        connect(widthSave, &QTimer::timeout, this, [this] {
            if (m_tabListPanel && !m_tabListPanel->isHidden())
                m_config.setTabListPanelWidth(m_tabSplitter->sizes().value(0));
        });
        connect(m_tabSplitter, &QSplitter::splitterMoved, widthSave,
                [widthSave](int, int) { widthSave->start(); });
    }
    if (m_tabListPanel) {
        m_tabListPanel->setVisible(show);
        if (show) {
            const int width = m_config.tabListPanelWidth();
            const int total = m_tabSplitter->width();
            m_tabSplitter->setSizes({width, qMax(1, total - width)});
            m_tabListPanel->setFontPointSize(m_config.fontSize());
            m_tabListPanel->setSortMode(
                TabListPanel::sortModeFromKey(m_config.tabListPanelSort()));
        }
    }
    updateTabBarVisibility();

    // The View menu ticks follow the config, including an external edit.
    if (m_tabListPanelAction) {
        const QSignalBlocker block(m_tabListPanelAction);
        m_tabListPanelAction->setChecked(show);
    }
    if (m_tabListHidesBarAction) {
        const QSignalBlocker block(m_tabListHidesBarAction);
        m_tabListHidesBarAction->setChecked(m_config.tabListPanelHidesTabBar());
    }
    if (m_tabListSortGroup) {
        const QString key =
            TabListPanel::sortModeKey(TabListPanel::sortModeFromKey(m_config.tabListPanelSort()));
        for (QAction *a : m_tabListSortGroup->actions()) {
            const QSignalBlocker block(a);
            a->setChecked(a->data().toString() == key);
        }
    }
    refreshTabList();
}

void MainWindow::showTabSwitcher() {
    if (!m_commandPalette || !m_tabWidget) return;
    delete m_tabSwitcherActions;
    m_tabSwitcherActions = new QObject(this);

    QFont f = font();
    f.setPointSize(m_config.fontSize());
    const int h = QFontMetrics(f).height();
    const QColor bg = m_chromeBg.isValid() ? m_chromeBg : palette().color(QPalette::Window);

    QList<QAction *> actions;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        TabListEntry e{w, m_tabFullTitles.value(w, m_tabWidget->tabText(i)),
                       m_coloredTabBar->tabColor(i), m_coloredTabBar->indicatorAt(i).glyph};
        auto *a = new QAction(e.title, m_tabSwitcherActions);
        a->setIcon(TabListPanel::tabIcon(e, h, bg, devicePixelRatioF()));
        const QString state = TabListPanel::stateLabel(e.glyph);
        if (!state.isEmpty()) a->setToolTip(e.title + QStringLiteral(", ") + state);
        const QPointer<QWidget> page(w);
        connect(a, &QAction::triggered, this, [this, page] {
            const int idx = page ? m_tabWidget->indexOf(page) : -1;
            if (idx >= 0) m_tabWidget->setCurrentIndex(idx);
        });
        actions << a;
    }
    m_commandPalette->showTabs(actions, m_config.fontSize());
}

void MainWindow::scheduleTabListRefresh() {
    if (m_tabListPanel && !m_tabListPanel->isHidden() && m_tabListRefreshTimer)
        m_tabListRefreshTimer->start();
}

void MainWindow::refreshTabList() {
    if (!m_tabListPanel || m_tabListPanel->isHidden() || !m_tabWidget) return;
    QVector<TabListEntry> entries;
    entries.reserve(m_tabWidget->count());
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        QWidget *w = m_tabWidget->widget(i);
        entries.push_back({w, m_tabFullTitles.value(w, m_tabWidget->tabText(i)),
                           m_coloredTabBar->tabColor(i),
                           m_coloredTabBar->indicatorAt(i).glyph});
    }
    m_tabListPanel->setEntries(entries, m_tabWidget->currentWidget());
}
