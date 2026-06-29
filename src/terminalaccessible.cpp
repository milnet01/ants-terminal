#include "terminalaccessible.h"
#include "terminalwidget.h"

#include <QTextBoundaryFinder>
#include <QWidget>

// ANTS-1078 — see docs/specs/ANTS-1078.md. This adapter is pure Qt-a11y
// glue; every content decision (viewport text, caret offset, cell rects)
// lives in TerminalWidget's accessible*() helpers.

namespace {

QTextBoundaryFinder::BoundaryType toFinderType(QAccessible::TextBoundaryType b) {
    switch (b) {
    case QAccessible::CharBoundary:     return QTextBoundaryFinder::Grapheme;
    case QAccessible::WordBoundary:     return QTextBoundaryFinder::Word;
    case QAccessible::SentenceBoundary: return QTextBoundaryFinder::Sentence;
    case QAccessible::LineBoundary:                       // a terminal row is
    case QAccessible::ParagraphBoundary: return QTextBoundaryFinder::Line;  // both
    case QAccessible::NoBoundary:       return QTextBoundaryFinder::Grapheme; // unused
    }
    return QTextBoundaryFinder::Grapheme;
}

} // namespace

TerminalWidgetAccessible::TerminalWidgetAccessible(QWidget *w)
    : QAccessibleWidget(w, QAccessible::Terminal, QStringLiteral("Terminal")) {}

TerminalWidget *TerminalWidgetAccessible::term() const {
    return qobject_cast<TerminalWidget *>(object());
}

void *TerminalWidgetAccessible::interface_cast(QAccessible::InterfaceType t) {
    if (t == QAccessible::TextInterface)
        return static_cast<QAccessibleTextInterface *>(this);
    return QAccessibleWidget::interface_cast(t);
}

QString TerminalWidgetAccessible::text(int startOffset, int endOffset) const {
    TerminalWidget *t = term();
    if (!t) return {};
    const QString s = t->accessibleText();
    if (endOffset < 0) endOffset = s.size();          // Qt convention: -1 => to end
    startOffset = qBound(0, startOffset, s.size());
    endOffset = qBound(0, endOffset, s.size());
    if (endOffset <= startOffset) return {};
    return s.mid(startOffset, endOffset - startOffset);
}

int TerminalWidgetAccessible::characterCount() const {
    TerminalWidget *t = term();
    return t ? t->accessibleText().size() : 0;
}

int TerminalWidgetAccessible::cursorPosition() const {
    TerminalWidget *t = term();
    return t ? t->accessibleCaretOffset() : 0;
}

QString TerminalWidgetAccessible::textAtOffset(int offset,
        QAccessible::TextBoundaryType boundaryType,
        int *startOffset, int *endOffset) const {
    const QString s = term() ? term()->accessibleText() : QString();
    if (boundaryType == QAccessible::NoBoundary) {
        *startOffset = 0; *endOffset = s.size(); return s;
    }
    offset = qBound(0, offset, s.size());
    QTextBoundaryFinder f(toFinderType(boundaryType), s);
    f.setPosition(offset);
    int start = f.isAtBoundary() ? offset : f.toPreviousBoundary();
    if (start < 0) start = 0;
    f.setPosition(start);
    int end = f.toNextBoundary();
    if (end < 0) end = s.size();
    *startOffset = start; *endOffset = end;
    return s.mid(start, end - start);
}

QString TerminalWidgetAccessible::textBeforeOffset(int offset,
        QAccessible::TextBoundaryType boundaryType,
        int *startOffset, int *endOffset) const {
    const QString s = term() ? term()->accessibleText() : QString();
    if (boundaryType == QAccessible::NoBoundary) {
        *startOffset = 0; *endOffset = 0; return {};
    }
    offset = qBound(0, offset, s.size());
    QTextBoundaryFinder f(toFinderType(boundaryType), s);
    f.setPosition(offset);
    int end = f.toPreviousBoundary();
    if (end <= 0) { *startOffset = 0; *endOffset = 0; return {}; }
    f.setPosition(end);
    int start = f.toPreviousBoundary();
    if (start < 0) start = 0;
    *startOffset = start; *endOffset = end;
    return s.mid(start, end - start);
}

QString TerminalWidgetAccessible::textAfterOffset(int offset,
        QAccessible::TextBoundaryType boundaryType,
        int *startOffset, int *endOffset) const {
    const QString s = term() ? term()->accessibleText() : QString();
    if (boundaryType == QAccessible::NoBoundary) {
        *startOffset = s.size(); *endOffset = s.size(); return {};
    }
    offset = qBound(0, offset, s.size());
    QTextBoundaryFinder f(toFinderType(boundaryType), s);
    f.setPosition(offset);
    int start = f.toNextBoundary();
    if (start < 0) { *startOffset = s.size(); *endOffset = s.size(); return {}; }
    f.setPosition(start);
    int end = f.toNextBoundary();
    if (end < 0) end = s.size();
    *startOffset = start; *endOffset = end;
    return s.mid(start, end - start);
}

QRect TerminalWidgetAccessible::characterRect(int offset) const {
    TerminalWidget *t = term();
    if (!t) return {};
    const QRect r = t->accessibleRectForOffset(offset);
    if (r.isNull()) return {};
    // mapToGlobal takes a QPoint (the QRect overload is Qt-6.7+); map the
    // top-left and keep the widget-space size.
    return QRect(t->mapToGlobal(r.topLeft()), r.size());
}

int TerminalWidgetAccessible::offsetAtPoint(const QPoint &point) const {
    TerminalWidget *t = term();
    if (!t) return -1;
    return t->accessibleOffsetAt(t->mapFromGlobal(point));
}

void TerminalWidgetAccessible::selection(int, int *startOffset, int *endOffset) const {
    *startOffset = -1;   // v1: no AT selection
    *endOffset = -1;
}

QString TerminalWidgetAccessible::attributes(int offset,
        int *startOffset, int *endOffset) const {
    const int n = characterCount();
    const int s0 = qBound(0, offset, n);
    *startOffset = s0;
    *endOffset = qMin(s0 + 1, n);
    return {};           // v1: no rich attributes (ANTS-3363)
}

void installTerminalAccessibilityFactory() {
    QAccessible::installFactory(
        [](const QString &classname, QObject *object) -> QAccessibleInterface * {
            if (object && object->isWidgetType()
                && classname == QLatin1String("TerminalWidget"))
                return new TerminalWidgetAccessible(static_cast<QWidget *>(object));
            return nullptr;
        });
}
