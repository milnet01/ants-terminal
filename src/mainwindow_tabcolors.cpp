// ANTS-1677 mainwindow piece 7/8 — tab colours
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "coloredtabbar.h"
#include "terminalwidget.h"
#include <QJsonObject>
#include <QLineEdit>
#include <QTabBar>
#include <QSplitter>
#include <QJsonArray>
#include <QCursor>
#include <QColorDialog>  // ANTS-1374 — custom per-tab colour picker
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <algorithm>

using namespace mainwindowdetail;

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

    // ANTS-1374 / ANTS-4689 — the tab-tag palette: 25 colours plus "None".
    // The 14 Catppuccin Mocha accents, ordered warm→cool, with six pastels
    // filling that row's hue gaps (Orchid ~308°, Indigo ~243°, Sand ~34°,
    // Olive ~56°, Lime ~82°, Mint ~150°) and the Mocha neutral ramp last —
    // Silver, Gray, Slate, Charcoal, Black — because a user asking for a
    // grey tab wants it beside the other greys, not sorted by hue among the
    // colours. Preset names kept stable (Purple = Mauve, Orange = Peach) so
    // existing users' colour vocabulary still matches.
    //
    // ON CONTRAST, because the previous note here was reassuring and wrong.
    // It said the added colours "can't be darker/lower-contrast than what
    // shipped" since all sit in one pastel band. Measured against Mocha base
    // #1E1E2E with text #CDD6F4, through the alpha-140 wash paintEvent()
    // composites OVER the label: the shipped pastels land at 2.50–2.9:1, and
    // the neutrals added here are the BEST in the palette (Gray 3.2, Black
    // 3.3, Charcoal 3.4) — a dark wash darkens the light text less than it
    // darkens the tab beneath it. So the band was never what protected
    // contrast, and lightness is the wrong axis to screen a candidate on.
    //
    // The real bar for a new entry: compute the ratio through the wash and
    // require it to be no worse than the WORST colour already shipped. Every
    // colour below clears that. Raising the floor for all of them is a
    // separate change (it would alter every existing tab's appearance) and
    // is ANTS-4690.
    // ANTS-5238 — the list itself lives in ColoredTabBar::palette(), shared
    // with View > Give Each Tab a Different Colour.
    struct ColorEntry { QString name; QColor color; };
    QList<ColorEntry> colors = {{"None", QColor()}};
    for (const ColoredTabBar::PaletteEntry &e : ColoredTabBar::palette())
        colors.append({e.name, e.color});
    // Show which colour the tab is on now. Without this the menu gives no
    // way to read the current state — the swatch is on the tab, but the tab
    // is behind the menu, and "None" is indistinguishable from any colour.
    //
    // An exclusive QActionGroup, so menus.md M1 applies: a "choose one"
    // radio pick keeps Qt's default toggle-and-close, which is what a colour
    // pick should do. (The StayOpenOnToggleFilter is for the menu BAR's
    // independent toggles and does not reach this context menu.)
    //
    // Compared on rgb() rather than with QColor::operator==: a persisted
    // colour is round-tripped through HexArgb and need not come back with
    // the same spec, and an equality that silently fails would leave the
    // menu looking exactly as it did before this change.
    const QColor currentColor =
        m_coloredTabBar ? m_coloredTabBar->tabColor(tabIndex) : QColor();
    auto sameColor = [](const QColor &a, const QColor &b) {
        if (!a.isValid() || !b.isValid()) return a.isValid() == b.isValid();
        return a.rgb() == b.rgb();
    };
    auto *colorGroup = new QActionGroup(&menu);
    colorGroup->setExclusive(true);
    bool matchedPreset = false;

    // The active entry needs a marker the STYLE cannot swallow. A checkable
    // QAction that also carries an icon renders its checked state as a
    // framed swatch in Breeze/Fusion — indistinguishable from the other 25
    // swatches — so setChecked() alone is invisible here, and the bold font
    // alone was too subtle to find in a 27-row menu (reported 2026-09-04
    // against a tab sitting on Lavender). Three cues now, and the text one
    // is the only one no style can drop and the only one the icon-less
    // "None" row can carry: a tick composited into the swatch, a "\u2713"
    // suffix on the label, and bold.
    auto tickedSwatch = [](const QColor &c) {
        QPixmap px(14, 14);
        px.fill(Qt::transparent);
        QPainter p(&px);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(QRect(0, 0, 14, 14), c);
        // Ink picked against the swatch itself, so the tick reads on both
        // the near-black Black preset and the near-white Rosewater one.
        const QColor ink = c.lightness() < 128 ? QColor(0xFF, 0xFF, 0xFF)
                                               : QColor(0x11, 0x11, 0x1B);
        QPen tick(ink, 2.0);
        tick.setCapStyle(Qt::RoundCap);
        tick.setJoinStyle(Qt::RoundJoin);
        p.setPen(tick);
        p.drawPolyline(QPolygonF(
            {QPointF(3.0, 7.5), QPointF(5.75, 10.25), QPointF(11.0, 4.25)}));
        return QIcon(px);
    };
    // Universal-character-name rather than a literal glyph: the escape is
    // decoded by the compiler into UTF-8 execution bytes, which fromUtf8
    // then reads back correctly.
    const QString kActiveSuffix = QString::fromUtf8("   \u2713");

    for (const auto &ce : colors) {
        QAction *a = menu.addAction(ce.name);
        const bool active = sameColor(ce.color, currentColor);
        if (ce.color.isValid()) {
            if (active) {
                a->setIcon(tickedSwatch(ce.color));
            } else {
                QPixmap px(12, 12);
                px.fill(ce.color);
                a->setIcon(QIcon(px));
            }
        }
        a->setCheckable(true);
        colorGroup->addAction(a);
        if (active) {
            a->setChecked(true);
            a->setText(ce.name + kActiveSuffix);
            QFont f = a->font();
            f.setBold(true);
            a->setFont(f);
            matchedPreset = true;
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

    // ANTS-1374 — arbitrary per-tab colour via QColorDialog. Routes through
    // the exact same in-session (setTabColor) + persist (persistTabColor)
    // path as the presets above, so a custom pick survives restart and
    // drag-reorder identically — persistTabColor already stores HexArgb.
    menu.addSeparator();
    QAction *customAction = menu.addAction("Custom colour...");
    // A colour set from the picker matches no preset, so mark THIS entry
    // instead — otherwise a custom-coloured tab shows nothing checked at
    // all, which reads as "None".
    customAction->setCheckable(true);
    colorGroup->addAction(customAction);
    if (currentColor.isValid() && !matchedPreset) {
        customAction->setChecked(true);
        customAction->setText(customAction->text() + kActiveSuffix);
        QFont f = customAction->font();
        f.setBold(true);
        customAction->setFont(f);
        customAction->setIcon(tickedSwatch(currentColor));
    }
    connect(customAction, &QAction::triggered, this, [this, tabWidget]() {
        int idx = m_tabWidget->indexOf(tabWidget);
        if (idx < 0 || !m_coloredTabBar) return;
        // Seed with the tab's current colour; an invalid QColor (uncoloured
        // tab) just makes the dialog open on its own default.
        const QColor chosen = QColorDialog::getColor(
            m_coloredTabBar->tabColor(idx), this, "Tab title background");
        if (!chosen.isValid()) return;  // user cancelled
        m_coloredTabBar->setTabColor(idx, chosen);
        persistTabColor(tabWidget, chosen);
    });

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

void MainWindow::colorTabsDistinctly() {
    if (!m_coloredTabBar) return;
    const QList<QColor> colours =
        ColoredTabBar::distinctColors(m_tabWidget->count());
    for (int i = 0; i < colours.size(); ++i) {
        m_coloredTabBar->setTabColor(i, colours.at(i));
        persistTabColor(m_tabWidget->widget(i), colours.at(i));
    }
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
