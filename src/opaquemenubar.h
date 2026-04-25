#pragma once

// QMenuBar subclass that explicitly fills its rect with an opaque
// background color in paintEvent before delegating to the base class.
//
// Why this exists: under Qt::WA_TranslucentBackground on the top-level
// window, none of the conventional opaque-paint paths are reliable on
// every WM/compositor combination KWin / Mutter / picom + Breeze /
// Fusion / qgnomeplatform):
//
//   - autoFillBackground is suppressed when WA_OpaquePaintEvent is set
//     on the same widget (the WA_OpaquePaintEvent contract is "the
//     widget paints all pixels," so Qt skips the auto-fill).
//   - QSS `QMenuBar { background-color: … }` paints via
//     QStyleSheetStyle::drawControl(CE_MenuBarEmptyArea), but on some
//     stacks this draw never runs when WA_OpaquePaintEvent is set —
//     the QSS engine assumes the widget has covered the empty-area
//     pixels itself.
//   - QPalette::Window is only consulted by autoFillBackground, so
//     it inherits the suppression above.
//
// User report 2026-04-25: every safeguard above was in place
// (autoFillBackground=true, palette Window=bgSecondary,
// WA_StyledBackground, WA_OpaquePaintEvent, top-level QSS *and*
// widget-local QSS with explicit background-color) and the menubar
// still rendered the desktop wallpaper through. The shared failure
// mode is "Qt skipped the background paint on this stack." The only
// way to keep the WA_OpaquePaintEvent contract honest under
// translucent parents is to actually paint the rect ourselves.
//
// We keep WA_OpaquePaintEvent on the widget (set by the construction
// site in mainwindow.cpp) — it's a hint to Qt's region tracking that
// suppresses the open-dropdown compositor-damage flicker on KWin
// (menubar_hover_stylesheet INV-3b). The override below is what
// makes that contract actually true.

#include <QMenuBar>
#include <QPaintEvent>
#include <QPainter>
#include <QColor>

class OpaqueMenuBar : public QMenuBar {
public:
    explicit OpaqueMenuBar(QWidget *parent = nullptr) : QMenuBar(parent) {}

    void setBackgroundFill(const QColor &c) {
        if (m_bg != c) {
            m_bg = c;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent *e) override {
        if (m_bg.isValid()) {
            QPainter p(this);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(rect(), m_bg);
        }
        QMenuBar::paintEvent(e);
    }

private:
    QColor m_bg;
};
