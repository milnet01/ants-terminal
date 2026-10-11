// ANTS-5645 — the side tab list panel: a list of every open tab by its
// full name, beside the tab widget. Contract:
// tests/features/tab_list_panel/spec.md. Reload: applyTabListPanelConfig()
// runs from both config-reload paths, so no setting needs a relaunch.

#include "mainwindow.h"

#include "coloredtabbar.h"
#include "tablistpanel.h"

#include <QAction>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>

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
        }
    }
    if (m_coloredTabBar)
        m_coloredTabBar->setVisible(!(show && m_config.tabListPanelHidesTabBar()));

    // The View menu ticks follow the config, including an external edit.
    if (m_tabListPanelAction) {
        const QSignalBlocker block(m_tabListPanelAction);
        m_tabListPanelAction->setChecked(show);
    }
    if (m_tabListHidesBarAction) {
        const QSignalBlocker block(m_tabListHidesBarAction);
        m_tabListHidesBarAction->setChecked(m_config.tabListPanelHidesTabBar());
    }
    refreshTabList();
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
