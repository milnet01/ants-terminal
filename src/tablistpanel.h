#pragma once

#include "coloredtabbar.h"

#include <QColor>
#include <QPointer>
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

    // ANTS-5656 — the row order. Sorting reorders the panel only, never
    // the tabs. The config key `tab_list_panel_sort` stores sortModeKey().
    enum class SortMode { ByColour, ByColourAZ, AZ, TabOrder };
    void setSortMode(SortMode mode);
    SortMode sortMode() const { return m_sort; }
    static QString sortModeKey(SortMode mode);
    // An unknown or empty key is ByColour.
    static SortMode sortModeFromKey(const QString &key);

    // An opaque background and the text colour, from the theme. Under the
    // window's translucent background an unfilled panel shows the desktop.
    void setThemeColors(const QColor &background, const QColor &text);

    // The group header for a tab colour: its palette name, or
    // "Custom colour". An invalid colour is "No colour".
    static QString groupName(const QColor &color);

    // The Claude state in words, for the tooltip and screen readers.
    // Empty for Glyph::None.
    static QString stateLabel(ClaudeTabIndicator::Glyph glyph);

    // The colour stripe and Claude dot for one tab, `height` pixels tall,
    // the dot's contrast taken against `background`. Shared with the
    // command palette's tab mode (ANTS-5646).
    static QIcon tabIcon(const TabListEntry &e, int height, const QColor &background,
                         qreal devicePixelRatio);

    // ANTS-5650 — a tab row's background and name colour: the Claude state
    // colour (the panel background for None), changed under the mouse, with
    // black or white ink, whichever reads better on it.
    // ANTS-5658 — at rest a state row fades left to right from `fill`,
    // under the name, to `fade`, the background. Hovered or None, `fade`
    // equals `fill`: one colour.
    struct RowPaint {
        QColor fill;
        QColor fade;
        QColor ink;
    };
    static RowPaint rowPaint(ClaudeTabIndicator::Glyph glyph, const QColor &background,
                             bool hovered);

    // Open an editor on `page`'s row, holding its whole title. Rows are
    // not rebuilt while it is open. Returns false when no row shows `page`.
    bool startRename(QWidget *page);

    QTreeWidget *treeForTest() const { return m_tree; }

signals:
    void tabActivated(QWidget *page);
    // A right-click on a tab row. Headers send nothing.
    void tabContextMenuRequested(QWidget *page);
    // The rename editor closed with a changed name. Escape sends nothing.
    void tabRenamed(QWidget *page, const QString &name);

private:
    void rebuild();
    void onItemActivated(QTreeWidgetItem *item);
    void onEditorClosed(bool submitted);

    QTreeWidget *m_tree = nullptr;
    QVector<TabListEntry> m_entries;
    QWidget *m_current = nullptr;
    int m_points = 0;
    SortMode m_sort = SortMode::ByColour;
    QTreeWidgetItem *m_renaming = nullptr;  // the row being renamed
    QString m_renameFrom;                   // its title when the editor opened
    QPointer<QWidget> m_renamePage;         // null if the tab closes mid-edit
};
