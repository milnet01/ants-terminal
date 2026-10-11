#pragma once

#include "coloredtabbar.h"

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

// ANTS-5645 — one tab as the side panel shows it. MainWindow builds the
// list from its tab widget; the panel stores nothing else about tabs.
struct TabListEntry {
    QWidget *page = nullptr;   // the tab's page widget, the switch target
    QString title;             // the whole title, never the cut tab text
    QColor color;              // the tab colour; invalid means none
    ClaudeTabIndicator::Glyph glyph = ClaudeTabIndicator::Glyph::None;

    bool operator==(const TabListEntry &o) const {
        return page == o.page && title == o.title && color == o.color
            && glyph == o.glyph;
    }
};

// ANTS-5645 — a side panel listing every open tab by its full name,
// grouped by tab colour, as an alternative to the scrolling tab bar.
// Contract: tests/features/tab_list_panel/spec.md.
class TabListPanel : public QWidget {
    Q_OBJECT

public:
    explicit TabListPanel(QWidget *parent = nullptr);

    // Replace the rows. `current` is the selected tab's page. An unchanged
    // call rebuilds nothing, so a frequent caller (Claude state changes)
    // keeps the scroll position and costs a comparison.
    void setEntries(const QVector<TabListEntry> &entries, QWidget *current);

    // The rows' font size, in points. MainWindow passes the terminal's.
    void setFontPointSize(int points);

    // An opaque background and the text colour, from the theme. Under the
    // window's translucent background an unfilled panel shows the desktop.
    void setThemeColors(const QColor &background, const QColor &text);

    // The group header for a tab colour: its palette name, or
    // "Custom colour". An invalid colour is "No colour".
    static QString groupName(const QColor &color);

    // The Claude state in words, for the tooltip and screen readers.
    // Empty for Glyph::None.
    static QString stateLabel(ClaudeTabIndicator::Glyph glyph);

    QTreeWidget *treeForTest() const { return m_tree; }

signals:
    void tabActivated(QWidget *page);

private:
    void rebuild();
    void onItemActivated(QTreeWidgetItem *item);
    QIcon rowIcon(const TabListEntry &e) const;

    QTreeWidget *m_tree = nullptr;
    QVector<TabListEntry> m_entries;
    QWidget *m_current = nullptr;
    int m_points = 0;
};
