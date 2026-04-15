#include "coloredtabbar.h"

#include <QPaintEvent>
#include <QPainter>

ColoredTabBar::ColoredTabBar(QWidget *parent) : QTabBar(parent) {}

ColoredTabWidget::ColoredTabWidget(QWidget *parent)
    : QTabWidget(parent), m_bar(new ColoredTabBar(this)) {
    setTabBar(m_bar);
}

void ColoredTabBar::setTabColor(int index, const QColor &color) {
    if (index < 0 || index >= count()) return;
    // An invalid QColor is Qt's idiom for "no colour" — store it so
    // tabData() round-trips correctly (and clears any prior value).
    setTabData(index, color.isValid() ? QVariant::fromValue(color) : QVariant());
    // Bottom-strip area of the changed tab is the only dirty region;
    // a conservative update() suffices — QTabBar coalesces these.
    update();
}

QColor ColoredTabBar::tabColor(int index) const {
    if (index < 0 || index >= count()) return QColor();
    const QVariant v = tabData(index);
    if (!v.isValid()) return QColor();
    return v.value<QColor>();
}

void ColoredTabBar::paintEvent(QPaintEvent *event) {
    // Let the base class draw the themed tabs first — shape, text,
    // selection state, hover highlight all come from the current style
    // + the app's stylesheet. We only contribute the gradient overlay.
    QTabBar::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    // SourceOver (default) lets the alpha-gradient composite over the
    // already-painted tab background without destroying the text the
    // base class rendered.

    for (int i = 0; i < count(); ++i) {
        const QColor c = tabColor(i);
        if (!c.isValid()) continue;

        const QRect r = tabRect(i);
        if (!event->region().intersects(r)) continue;

        // 0.6.26 — replace the old 3px bottom strip with a full vertical
        // gradient from transparent (top) to the chosen colour (bottom).
        // User request: "please give it a light gradient top (transparent)
        // to bottom (chosen colour) and it must still have the line
        // underneath to show which tab is active."
        //
        // The app stylesheet (mainwindow.cpp:1678-1680) gives every tab a
        // 2px transparent bottom border and the *selected* tab a 2px
        // accent-colour bottom border. That 2px strip is the "line
        // underneath" that signals the active tab, so we exclude it from
        // the gradient area — painting into it would either overwrite
        // the accent line (selected tab) or manufacture a fake one on
        // non-selected tabs.
        //
        // Bottom alpha of 140/255 is the sweet spot on a 30px tab: tab
        // text stays readable across all themes (tested against Dark,
        // Solarized-light, Gruvbox) while the colour is clearly visible.
        // Top alpha 0 makes the top edge of the tab look untinted so the
        // gradient reads as an intentional wash rather than a full fill.
        constexpr int kActiveUnderlineReserve = 2;
        const QRect gradRect(r.left(), r.top(),
                             r.width(), r.height() - kActiveUnderlineReserve);
        if (gradRect.height() <= 0) continue;

        QLinearGradient g(gradRect.topLeft(), gradRect.bottomLeft());
        QColor topStop = c; topStop.setAlpha(0);
        QColor botStop = c; botStop.setAlpha(140);
        g.setColorAt(0.0, topStop);
        g.setColorAt(1.0, botStop);
        painter.fillRect(gradRect, g);
    }
}
