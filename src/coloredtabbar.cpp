#include "coloredtabbar.h"

#include <QApplication>
#include <QIcon>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>

#include <cmath>

namespace {

// Object name tagging our custom close buttons so installCloseButton can
// tell its own button (re-tint path) from Qt's built-in one / nothing.
constexpr QLatin1String kCloseBtnName("antsTabClose");

// Build a two-line × close glyph as a QIcon with a Normal (resting) and
// Active (hover) tint. Rendered at the widget's device-pixel-ratio so the
// strokes stay crisp on HiDPI. The glyph is a 16-logical-px box with a
// ~3px inset; setIconSize(12) on the button trims the visual to ~10px.
QIcon makeCloseIcon(const QColor &normal, const QColor &hover) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    auto draw = [dpr](const QColor &c) {
        QPixmap pm(QSize(16, 16) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(c, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        // Logical coords (QPainter scales by the pixmap's dpr).
        p.drawLine(4, 4, 12, 12);
        p.drawLine(12, 4, 4, 12);
        return pm;
    };
    QIcon ic;
    ic.addPixmap(draw(normal), QIcon::Normal);
    ic.addPixmap(draw(hover), QIcon::Active);
    return ic;
}

// WCAG 2.x relative luminance + contrast ratio. Drives the light-theme
// contrast adaptation of the Claude state dots (ANTS-1847): the fixed
// dark-tuned palette washes out on near-white tab backgrounds, so the
// dot/label lightness is lowered until it clears the 3:1 non-text floor.
double wcagLuminance(const QColor &c) {
    auto lin = [](int v8) {
        const double v = v8 / 255.0;
        return v <= 0.03928 ? v / 12.92
                            : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) +
           0.0722 * lin(c.blue());
}
double wcagContrast(const QColor &a, const QColor &b) {
    const double la = wcagLuminance(a) + 0.05;
    const double lb = wcagLuminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}
constexpr double kMinContrast = 3.0;  // WCAG 1.4.11 non-text contrast.
}  // namespace

// Single source of truth for the per-state Claude palette. Hex values
// match `tests/features/claude_state_dot_palette/spec.md` and are
// theme-independent — state identity is the contract, not the
// surrounding theme. Red is intentionally absent: AwaitingInput is a
// normal interaction state, not an error.
QColor ClaudeTabIndicator::color(Glyph g) {
    switch (g) {
        case Glyph::None:           return QColor();             // invalid → callers skip
        case Glyph::Idle:           return QColor("#888888");    // grey
        case Glyph::Thinking:       return QColor("#5BA0E5");    // blue
        case Glyph::ToolUse:        return QColor("#E5C24A");    // yellow
        case Glyph::Bash:           return QColor("#6FCF50");    // green
        case Glyph::Planning:       return QColor("#5DCFCF");    // cyan
        case Glyph::Auditing:       return QColor("#C76DC7");    // magenta
        case Glyph::Compacting:     return QColor("#A87FE0");    // violet
        case Glyph::AwaitingInput:  return QColor("#F08A4B");    // orange
    }
    return QColor();
}

QColor ClaudeTabIndicator::contrastColor(Glyph g, const QColor &background) {
    const QColor base = color(g);
    if (!base.isValid() || !background.isValid()) return base;
    if (wcagContrast(base, background) >= kMinContrast) return base;

    // The base palette is dark-tuned; on a light theme the dot is too
    // pale. Preserve hue + saturation (state identity) and only lower HSL
    // lightness until the dot clears the 3:1 floor. Contrast is monotonic
    // in lightness against a light background, so binary-search the
    // highest (least-darkened) lightness <= the original that still
    // passes — black always clears a light background, so a solution
    // exists. State hue is unchanged, so "orange = needs me" still holds.
    //
    // Precondition (true for all 11 shipped themes): every theme's
    // bgSecondary is either clearly light (lum > 0.8 — darkening the dot
    // is the correct direction) or clearly dark (lum < 0.05 — the base
    // already passes 3:1 and we early-returned above). No theme sits in
    // the mid-luminance band where the dot would instead need lightening;
    // a future mid-luminance theme would need this to search both
    // directions.
    const int h = qMax(0, base.hslHue());  // -1 (achromatic grey) -> 0
    const int s = base.hslSaturation();
    const int a = base.alpha();
    int lo = 0, hi = base.lightness(), best = 0;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (wcagContrast(QColor::fromHsl(h, s, mid, a), background) >=
            kMinContrast) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return QColor::fromHsl(h, s, best, a);
}

QColor ClaudeTabIndicator::ringColor(const QColor &background) {
    if (!background.isValid()) return QColor(0, 0, 0, 160);
    // Theme-tinted, theme-adapted edge: keep the background's own hue +
    // saturation but push its lightness to the opposite end of the range,
    // so the ring is dark on a light theme and light on a dark theme while
    // still reading as part of the selected palette (ANTS-1847). Alpha
    // keeps it a subtle outline, not a hard stroke. State-independent —
    // the same ring wraps every dot.
    const int h = qMax(0, background.hslHue());
    const int s = background.hslSaturation();
    const int l = wcagLuminance(background) > 0.4 ? 40 : 220;
    QColor ring = QColor::fromHsl(h, s, l);
    ring.setAlpha(160);
    return ring;
}

QString ClaudeTabIndicator::glyphName(Glyph g) {
    switch (g) {
        case Glyph::None:           return QString();
        case Glyph::Idle:           return QStringLiteral("idle");
        case Glyph::Thinking:       return QStringLiteral("thinking");
        case Glyph::ToolUse:        return QStringLiteral("tool use");
        case Glyph::Bash:           return QStringLiteral("bash");
        case Glyph::Planning:       return QStringLiteral("planning");
        case Glyph::Auditing:       return QStringLiteral("auditing");
        case Glyph::Compacting:     return QStringLiteral("compacting");
        case Glyph::AwaitingInput:  return QStringLiteral("awaiting input");
    }
    return QString();
}

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

void ColoredTabBar::setCloseGlyphColors(const QColor &normal,
                                        const QColor &hover,
                                        const QColor &hoverBg) {
    m_closeNormal = normal;
    m_closeHover = hover;
    m_closeHoverBg = hoverBg;
    for (int i = 0; i < count(); ++i) installCloseButton(i);
}

void ColoredTabBar::tabInserted(int index) {
    QTabBar::tabInserted(index);
    installCloseButton(index);
}

void ColoredTabBar::installCloseButton(int index) {
    if (index < 0 || index >= count()) return;
    const auto side = static_cast<QTabBar::ButtonPosition>(style()->styleHint(
        QStyle::SH_TabBar_CloseButtonPosition, nullptr, this));

    // Reuse our button if it's already there (theme re-tint path); only
    // construct one when Qt's built-in close button (or nothing) sits in
    // the slot. setTabButton() takes ownership and deletes the displaced
    // widget, so we never leak Qt's CloseButton.
    auto *btn = qobject_cast<QToolButton *>(tabButton(index, side));
    if (!btn || btn->objectName() != kCloseBtnName) {
        btn = new QToolButton(this);
        btn->setObjectName(kCloseBtnName);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::ArrowCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setFixedSize(16, 16);
        btn->setIconSize(QSize(12, 12));
        // a11y: Qt's built-in close button carried a translated "Close
        // Tab" accessible name (see a11y_chrome_names spec); preserve it
        // for AT-SPI / Orca now that we own the widget.
        btn->setAccessibleName(tr("Close Tab"));
        btn->setToolTip(tr("Close Tab"));
        connect(btn, &QToolButton::clicked, this, [this, btn]() {
            // Resolve the button's CURRENT tab at click time — tab moves
            // and closes reshuffle indices, and Qt keeps the button paired
            // with its tab, so a captured index would go stale.
            const auto s = static_cast<QTabBar::ButtonPosition>(
                style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition,
                                   nullptr, this));
            for (int i = 0; i < count(); ++i)
                if (tabButton(i, s) == btn) {
                    emit tabCloseRequested(i);
                    return;
                }
        });
        setTabButton(index, side, btn);
    }

    // Tint only once colours have been supplied (applyTheme runs at
    // startup, so a tab added before the first theme apply is briefly
    // icon-less, then picked up by setCloseGlyphColors).
    if (m_closeNormal.isValid()) {
        btn->setIcon(makeCloseIcon(m_closeNormal, m_closeHover));
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; background: transparent; padding: 0; }"
            "QToolButton:hover { background: %1; border-radius: 3px; }")
            .arg(m_closeHoverBg.name()));
    }
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

    // Second pass: Claude Code per-tab state dot. Drawn on top of the
    // gradient, ~8 px circle at the leading edge of each tab. Provider
    // returns Glyph::None for tabs with no Claude process — those tabs
    // pay nothing here beyond the callback roundtrip. Colour is the
    // ONLY differentiator between states (no outlines, no per-state
    // radius variation, no badges) — see
    // `tests/features/claude_state_dot_palette/spec.md`.
    if (!m_indicatorProvider) return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    constexpr int kDotRadius = 4;
    for (int i = 0; i < count(); ++i) {
        const ClaudeTabIndicator ind = m_indicatorProvider(i);
        // ANTS-1185: expose the Claude state via the tab tooltip so
        // AT-SPI / Orca / Windows Narrator can announce it alongside
        // the tab title. QTabBar in Qt 6 doesn't surface a direct
        // setTabAccessibleName; tooltips fill that role (screen
        // readers honour tab tooltips). None blanks the tooltip;
        // non-None reads "Claude: <state>". Cheap — only fires when
        // the string actually changes.
        const QString stateName = ClaudeTabIndicator::glyphName(ind.glyph);
        const QString tip = stateName.isEmpty()
            ? QString()
            : (QStringLiteral("Claude: ") + stateName);
        if (tabToolTip(i) != tip)
            setTabToolTip(i, tip);

        // ANTS-1847 — contrast-adapt the fill against the tab background
        // so the hue stays perceptible on light themes (the base palette
        // is dark-tuned). m_bg is theme.bgSecondary, the surface the dot
        // sits on.
        const QColor fill = ClaudeTabIndicator::contrastColor(ind.glyph, m_bg);
        if (!fill.isValid()) continue;  // None / unrecognised → no dot

        const QRect r = tabRect(i);
        if (!event->region().intersects(r)) continue;

        // Tab padding is 22 px on the left (see the app stylesheet's
        // `QTabBar::tab { padding: 6px 16px 6px 22px; }` — bumped from
        // 16 in 0.7.48 after user feedback the dot crowded the text).
        // Center the dot in that gutter so it sits at ~11 px from the
        // edge and clears the first character of the tab text by ~7 px.
        const int cx = r.left() + 11;
        const int cy = r.center().y();
        // ANTS-1847 — uniform theme-adapted ring on every dot (same for
        // all states), so the dot keeps a crisp edge on any background.
        painter.setPen(QPen(ClaudeTabIndicator::ringColor(m_bg), 1));
        painter.setBrush(fill);
        painter.drawEllipse(QPoint(cx, cy), kDotRadius, kDotRadius);
    }
}
