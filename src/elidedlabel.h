#pragma once

// ElidedLabel — QLabel that automatically truncates its text with "…" to
// fit the current widget width. The full (un-elided) string is kept
// internally and re-elided whenever the widget is resized, so status-bar
// labels stop growing unbounded and pushing siblings off-screen.
//
// Usage:
//   auto *lbl = new ElidedLabel(this);
//   lbl->setElideMode(Qt::ElideRight);
//   lbl->setMaximumWidth(200);          // optional hard cap
//   lbl->setFullText("long text …");    // stores + elides to current width
//
// Why not override QLabel::setText? setText is non-virtual, so calling it
// via a QLabel* pointer would skip the elision path. setFullText is
// explicit at every call site and avoids the hiding-non-virtual footgun.
// The widget also exposes a full-text tooltip whenever the displayed text
// had to be elided, so hover-inspection always reveals the whole string.

#include <QLabel>
#include <QResizeEvent>

class ElidedLabel : public QLabel {
public:
    using QLabel::QLabel;

    void setFullText(const QString &text) {
        if (m_fullText == text) return;
        m_fullText = text;
        updateElided();
    }

    const QString &fullText() const { return m_fullText; }

    void setElideMode(Qt::TextElideMode mode) {
        if (m_elideMode == mode) return;
        m_elideMode = mode;
        updateElided();
    }

    Qt::TextElideMode elideMode() const { return m_elideMode; }

    // Minimum width policy: never compress the widget below what it takes
    // to render the full text, *unless* the full text is wider than
    // maximumWidth() — at which point elision kicks in and the minimum is
    // the cap width. Without this, QStatusBar's QBoxLayout was free to
    // squeeze the branch chip down to 3-char width ("…") even when the
    // statusbar had plenty of space; the user reported seeing "…" where
    // their branch name (6 chars) should have fit in ~50 px. Short text
    // must always show in full; only genuinely-over-cap text elides.
    //
    // CRITICAL: base the size on `m_fullText`, not on `QLabel::sizeHint()`
    // which reports the *displayed* text's width. Once elision has
    // already run, displayed is "…" and sizeHint is tiny — so relying on
    // it creates a chicken-and-egg loop where the widget shrinks itself
    // and then can't grow back. Compute from the full text directly via
    // fontMetrics. Include the stylesheet-style padding via contentsMargins
    // is unreliable across Qt versions; instead we add a small fixed
    // fudge (sum of two averageCharWidth) to cover the typical
    // padding: 1px 8px from the chip stylesheet plus a safety margin.
    QSize minimumSizeHint() const override {
        const QFontMetrics fm(font());
        const int textW = fm.horizontalAdvance(m_fullText);
        const int padding = fm.averageCharWidth() * 2;  // ~16 px fudge
        int w = textW + padding;
        const int cap = maximumWidth();
        if (cap > 0 && cap < QWIDGETSIZE_MAX && w > cap) {
            w = cap;
        }
        // Also respect a minimum floor of 3 averageChar widths (the pre-fix
        // behavior) for safety when fullText is empty — the widget should
        // still be a non-zero width placeholder rather than disappearing
        // entirely in a stretch layout.
        const int floor = fm.averageCharWidth() * 3;
        if (w < floor) w = floor;
        return QSize(w, fm.height());
    }

    QSize sizeHint() const override {
        // Like minimumSizeHint, base sizeHint on the full text's width
        // (plus padding fudge) so the layout gives us enough room to
        // render without elision when space allows. QLabel's default
        // sizeHint reports the *displayed* text which has already been
        // elided — capturing that becomes a fixed point the widget
        // can never escape.
        const QFontMetrics fm(font());
        const int textW = fm.horizontalAdvance(m_fullText);
        const int padding = fm.averageCharWidth() * 2;
        int w = textW + padding;
        const int cap = maximumWidth();
        if (cap > 0 && cap < QWIDGETSIZE_MAX && w > cap) {
            w = cap;
        }
        return QSize(w, fm.height());
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        updateElided();
    }

private:
    void updateElided() {
        int w = contentsRect().width();
        // Subtract any stylesheet-applied padding that isn't reflected in
        // contentsRect yet (pre-first-layout). A small fudge keeps "…" from
        // sitting right against the right edge on narrow labels.
        if (w <= 0) w = width();
        QString displayed = m_fullText;
        if (w > 0 && !m_fullText.isEmpty()) {
            displayed = fontMetrics().elidedText(m_fullText, m_elideMode, w);
        }
        QLabel::setText(displayed);
        setToolTip(displayed != m_fullText ? m_fullText : QString());
    }

    QString m_fullText;
    Qt::TextElideMode m_elideMode = Qt::ElideRight;
};
