#pragma once

// QStatusBar subclass that explicitly fills its rect with an opaque
// background color in paintEvent before delegating to the base class.
//
// Why this exists: same translucent-parent failure mode documented in
// opaquemenubar.h. Under Qt::WA_TranslucentBackground on the top-level
// window, QSS `QStatusBar { background-color: ... }` paints via
// QStyleSheetStyle but Qt silently drops the empty-area fill on at
// least KWin + Breeze + Qt 6 once WA_OpaquePaintEvent is set on the
// widget — leaving the desktop wallpaper showing through the bar
// strip. autoFillBackground / WA_StyledBackground / palette Window
// inherit the same suppression. The only path that reliably keeps
// the WA_OpaquePaintEvent contract honest under translucent parents
// is to actually paint the rect ourselves.
//
// Mirror of OpaqueMenuBar — see opaquemenubar.h for the long-form
// rationale and the user report that drove the original menubar fix.
// User report 2026-04-25 (post-0.7.32) flagged the same symptom on
// the status bar strip and on the tab-bar empty area.

#include <QStatusBar>
#include <QPaintEvent>
#include <QPainter>
#include <QColor>

class OpaqueStatusBar : public QStatusBar {
public:
    explicit OpaqueStatusBar(QWidget *parent = nullptr) : QStatusBar(parent) {}

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
        QStatusBar::paintEvent(e);
    }

private:
    QColor m_bg;
};
