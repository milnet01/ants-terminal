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

    QString fullText() const { return m_fullText; }

    void setElideMode(Qt::TextElideMode mode) {
        if (m_elideMode == mode) return;
        m_elideMode = mode;
        updateElided();
    }

    Qt::TextElideMode elideMode() const { return m_elideMode; }

    // Let the label shrink; QLabel's default minimumSizeHint() is the width
    // of the full text, which defeats elision inside a stretch layout.
    QSize minimumSizeHint() const override {
        QSize s = QLabel::minimumSizeHint();
        // Room for "…" plus a couple of glyphs — prevents the widget from
        // collapsing to zero width, which would hide it entirely.
        int minW = fontMetrics().averageCharWidth() * 3;
        return QSize(minW, s.height());
    }

    QSize sizeHint() const override {
        // Cap preferred width at the un-elided text width, but don't force
        // growth beyond maximumWidth() (Qt clamps automatically).
        QSize s = QLabel::sizeHint();
        return s;
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
