#pragma once

#include <QTabBar>
#include <QTabWidget>

#include <functional>

class QPaintEvent;

// Per-tab Claude Code state overlay. Read by ColoredTabBar::paintEvent
// via a caller-supplied provider callback — the bar itself stores
// nothing, so tab reorder / close don't need to update any tab-bar
// side-table (the provider just returns the current value for the
// tab's underlying session identity, whatever the caller uses).
struct ClaudeTabIndicator {
    // Which dot to draw. NotRunning means "no glyph" — paint skipped.
    // Bash is split out from generic ToolUse because the roadmap calls
    // it out as a distinct state (it's the tool most likely to be
    // running long-lived commands), and because the bottom status bar
    // already distinguishes "Claude: bash" from "Claude: reading a
    // file" / "Claude: editing a file" / etc.
    enum class Glyph {
        None,            // no Claude process under this tab's shell
        Idle,            // muted dot
        Thinking,        // plain dot
        ToolUse,         // solid dot (neutral hue) — Read/Write/Edit/Grep/Glob/…
        Bash,            // Bash tool specifically — separate hue
        Planning,        // plan-mode active (distinct hue)
        Compacting,      // /compact in flight
        AwaitingInput,   // PermissionRequest pending — LOUD
    };
    Glyph glyph = Glyph::None;
};

// QTabBar subclass that renders an optional per-tab colour strip along
// the bottom edge of each tab. Used for visually tagging tabs into
// groups (red = prod, green = dev, …) à la Chrome.
//
// Storage lives in Qt's per-tab user data (setTabData / tabData) keyed
// as `tabColorRole()` — this survives tab reorder (drag-and-drop) and
// close without manual bookkeeping, which a MainWindow-held
// QHash<index, QColor> does not.
//
// Painting is additive: the base class paints the themed tab (shape +
// text + selection state) first, then paintEvent() overlays a 3-pixel
// bottom strip in the stored colour for any tab that has one. This
// approach sidesteps the stylesheet-vs-setTabTextColor precedence
// problem where QTabBar::tab { color: … } in the window stylesheet
// wins over a call to setTabTextColor and a user's choice never
// renders.
class ColoredTabBar : public QTabBar {
    Q_OBJECT

public:
    explicit ColoredTabBar(QWidget *parent = nullptr);

    // Set (or clear, when `color` is invalid) the colour strip on the
    // tab at `index`. Safe to call with an out-of-range index (no-op).
    void setTabColor(int index, const QColor &color);

    // Current stored colour for the tab at `index`, or an invalid QColor
    // if none is set or `index` is out of range.
    QColor tabColor(int index) const;

    // Supply a provider callback invoked during paintEvent for every
    // tab. Returning a Glyph other than `None` renders a small dot at
    // the tab's leading edge. The bar stores no state — invalidate via
    // QWidget::update() when the underlying state changes.
    using IndicatorProvider = std::function<ClaudeTabIndicator(int)>;
    void setClaudeIndicatorProvider(IndicatorProvider provider) {
        m_indicatorProvider = std::move(provider);
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static constexpr int kTabDataColorRole = 0x100;  // QTabBar reserves low ints

    IndicatorProvider m_indicatorProvider;
};

// Trivial QTabWidget subclass whose sole purpose is to install a
// ColoredTabBar at construction time — QTabWidget::setTabBar() is
// protected, so MainWindow can't pass the custom bar in directly. The
// bar pointer is cached and exposed via coloredTabBar() so callers can
// invoke setTabColor() without an extra dynamic_cast on every use.
class ColoredTabWidget : public QTabWidget {
    Q_OBJECT

public:
    explicit ColoredTabWidget(QWidget *parent = nullptr);

    ColoredTabBar *coloredTabBar() const { return m_bar; }

private:
    ColoredTabBar *m_bar;
};
