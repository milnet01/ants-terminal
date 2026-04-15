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
    // + the app's stylesheet. We only contribute the optional strip.
    QTabBar::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    for (int i = 0; i < count(); ++i) {
        const QColor c = tabColor(i);
        if (!c.isValid()) continue;

        const QRect r = tabRect(i);
        if (!event->region().intersects(r)) continue;

        // 3-pixel strip along the bottom of the tab, inset a few pixels
        // horizontally so it reads as a badge rather than a border that
        // butts up against adjacent tabs. Bottom-aligned because the
        // tab bar's selection line is typically top or underline-style
        // on most themes; a bottom strip reads as distinct decoration.
        constexpr int kStripHeight = 3;
        constexpr int kHorizInset = 6;
        const QRect strip(r.left() + kHorizInset,
                          r.bottom() - kStripHeight + 1,
                          r.width() - 2 * kHorizInset,
                          kStripHeight);
        if (strip.width() <= 0) continue;
        painter.fillRect(strip, c);
    }
}
