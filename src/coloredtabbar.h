#pragma once

#include <QColor>
#include <QTabBar>
#include <QTabWidget>

#include <functional>

class QPaintEvent;

// Per-tab Claude Code state overlay. Read by ColoredTabBar::paintEvent
// via a caller-supplied provider callback — the bar itself stores
// nothing, so tab reorder / close don't need to update any tab-bar
// side-table (the provider just returns the current value for the
// tab's underlying session identity, whatever the caller uses).
//
// Palette is uniform across both the per-tab dot and the bottom
// status-bar Claude label: callers retrieve the canonical colour for a
// state via `ClaudeTabIndicator::color(Glyph)` so the two surfaces stay
// in sync. See `tests/features/claude_state_dot_palette/spec.md` for
// the contract.
struct ClaudeTabIndicator {
    // Which dot to draw. None means "no glyph" — paint skipped.
    // Bash is split out from generic ToolUse because Bash is the tool
    // most likely to be running long-lived commands; the bottom status
    // bar already distinguishes "Claude: bash" from "Claude: reading a
    // file" / "Claude: editing a file" / etc., and the per-tab dot
    // mirrors that.
    enum class Glyph {
        None,            // no Claude process under this tab's shell
        Idle,            // grey
        Thinking,        // blue
        ToolUse,         // yellow — Read/Write/Edit/Grep/Glob/Task/…
        Bash,            // green — Bash tool specifically
        Planning,        // cyan — plan-mode active
        Auditing,        // magenta — /audit skill in flight
        Compacting,      // violet — /compact in flight
        AwaitingInput,   // orange — PermissionRequest pending
    };
    Glyph glyph = Glyph::None;

    // Single source of truth for the state→colour palette. Same hex
    // values drive both the tab-bar dot fill and the bottom status-bar
    // "Claude: …" label colour. None / unrecognised input returns an
    // invalid QColor so callers can suppress paint with `isValid()`.
    static QColor color(Glyph g);
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

    // Opaque background fill painted at the start of paintEvent, before
    // the base class draws tabs and before the colour-group gradient
    // overlay. Matches the OpaqueMenuBar pattern — under a translucent
    // parent (WA_TranslucentBackground on the top-level), the QSS
    // `QTabBar { background-color: ... }` rule is unreliable: KWin +
    // Breeze + Qt 6 silently drops the empty-area fill once
    // WA_OpaquePaintEvent is set on the widget, leaving the desktop
    // showing through the bar strip to the right of the last tab. The
    // explicit fillRect here is the only path Qt actually honors. See
    // opaquemenubar.h for the long-form rationale.
    void setBackgroundFill(const QColor &c);

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
    QColor m_bg;  // opaque fill, see setBackgroundFill comment
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
