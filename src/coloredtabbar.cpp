#include "coloredtabbar.h"

#include <QPaintEvent>
#include <QPainter>

ColoredTabBar::ColoredTabBar(QWidget *parent) : QTabBar(parent) {}

ColoredTabWidget::ColoredTabWidget(QWidget *parent)
    : QTabWidget(parent), m_bar(new ColoredTabBar(this)) {
    setTabBar(m_bar);
}

void ColoredTabBar::setBackgroundFill(const QColor &c) {
    if (m_bg != c) {
        m_bg = c;
        update();
    }
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
    // Opaque background fill — must run BEFORE the base class so the
    // tabs paint over the fill, not under it. CompositionMode_Source
    // overwrites whatever the compositor left in those pixels (the
    // desktop wallpaper, under WA_TranslucentBackground), guaranteeing
    // the bar strip to the right of the last tab is opaque even when
    // the QSS `QTabBar { background-color: ... }` rule is dropped by
    // Qt's stylesheet engine on translucent-parent stacks. See
    // setBackgroundFill comment in the header for why QSS alone is
    // not enough here.
    if (m_bg.isValid()) {
        QPainter bgPainter(this);
        bgPainter.setCompositionMode(QPainter::CompositionMode_Source);
        bgPainter.fillRect(rect(), m_bg);
    }

    // Let the base class draw the themed tabs — shape, text, selection
    // state, hover highlight all come from the current style + the
    // app's stylesheet. We only contribute the gradient overlay.
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

    // Second pass: Claude Code per-tab state glyph. Drawn on top of the
    // gradient, ~8 px dot at the leading edge of each tab. Provider
    // returns Glyph::None for tabs with no Claude process — those tabs
    // pay nothing here beyond the callback roundtrip.
    if (!m_indicatorProvider) return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < count(); ++i) {
        const ClaudeTabIndicator ind = m_indicatorProvider(i);
        if (ind.glyph == ClaudeTabIndicator::Glyph::None) continue;

        const QRect r = tabRect(i);
        if (!event->region().intersects(r)) continue;

        // Palette chosen against the ANSI-style gradient backdrop:
        // - Idle / Thinking: muted greys so the glyph is visible but
        //   doesn't fight for attention on a quiescent tab.
        // - ToolUse: blue — "Claude is doing something".
        // - Planning: cyan-teal — distinguishes from ToolUse without
        //   reading as an error.
        // - Compacting: violet — temporary, rare, informative.
        // - AwaitingInput: loud orange/red with a white outline so it
        //   catches peripheral vision even with the tab backgrounded.
        QColor fill;
        QColor outline;
        int radius = 4;
        switch (ind.glyph) {
            case ClaudeTabIndicator::Glyph::None:
                continue;
            case ClaudeTabIndicator::Glyph::Idle:
                fill = QColor(120, 120, 120);  // muted grey
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::Thinking:
                fill = QColor(180, 180, 180);  // light grey
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::ToolUse:
                fill = QColor(80, 160, 230);   // blue
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::Bash:
                fill = QColor(120, 200, 80);   // green — "shell is executing"
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::Planning:
                fill = QColor(70, 200, 200);   // cyan
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::Compacting:
                fill = QColor(170, 120, 220);  // violet
                outline = QColor(0, 0, 0, 0);
                break;
            case ClaudeTabIndicator::Glyph::AwaitingInput:
                fill = QColor(255, 120, 60);   // bright orange
                outline = QColor(255, 255, 255, 220);
                radius = 5;                    // slightly larger
                break;
        }

        // Tab padding is 16 px on the left (see the app stylesheet's
        // `QTabBar::tab { padding: 6px 16px; }`). Center the dot in that
        // gutter so it sits at ~8 px from the edge and clears the first
        // character of the tab text by ~6 px.
        const int cx = r.left() + 8;
        const int cy = r.center().y();
        if (outline.alpha() > 0) {
            painter.setPen(QPen(outline, 1.5));
        } else {
            painter.setPen(Qt::NoPen);
        }
        painter.setBrush(fill);
        painter.drawEllipse(QPoint(cx, cy), radius, radius);
    }
}
