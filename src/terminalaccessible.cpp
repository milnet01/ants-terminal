#include "terminalaccessible.h"
#include "terminalwidget.h"

#include <QTextBoundaryFinder>
#include <QWidget>

#include <utility>

// ANTS-1078 — see docs/specs/ANTS-1078-screen-reader-accessibility.md. This adapter is pure Qt-a11y
// glue; every content decision (viewport text, caret offset, cell rects)
// lives in TerminalWidget's accessible*() helpers.

namespace {

QTextBoundaryFinder::BoundaryType toFinderType(QAccessible::TextBoundaryType b) {
    switch (b) {
    case QAccessible::CharBoundary:     return QTextBoundaryFinder::Grapheme;
    case QAccessible::WordBoundary:     return QTextBoundaryFinder::Word;
    case QAccessible::SentenceBoundary: return QTextBoundaryFinder::Sentence;
    // Line and paragraph never reach the finder: see isRowBoundary.
    case QAccessible::LineBoundary:
    case QAccessible::ParagraphBoundary:
    case QAccessible::NoBoundary:       return QTextBoundaryFinder::Grapheme; // unused
    }
    return QTextBoundaryFinder::Grapheme;
}

// ANTS-6654 — a line or paragraph is one terminal row: the text up to and
// including its '\n'. QTextBoundaryFinder::Line finds wrap opportunities,
// which gave a screen reader a word-sized fragment.
bool isRowBoundary(QAccessible::TextBoundaryType b) {
    return b == QAccessible::LineBoundary || b == QAccessible::ParagraphBoundary;
}

// The row holding `offset` as [start, end); an offset on a '\n' belongs to
// the row that '\n' ends.
std::pair<int, int> rowAt(const QString &s, int offset) {
    const int start = offset > 0 ? int(s.lastIndexOf(QLatin1Char('\n'), offset - 1)) + 1 : 0;
    const int nl = int(s.indexOf(QLatin1Char('\n'), offset));
    return {start, nl < 0 ? int(s.size()) : nl + 1};
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
    QString s = term() ? term()->accessibleText() : QString();
    if (boundaryType == QAccessible::NoBoundary) {
        *startOffset = 0; *endOffset = s.size(); return s;
    }
    offset = qBound(0, offset, s.size());
    if (isRowBoundary(boundaryType)) {
        const auto [start, end] = rowAt(s, offset);
        *startOffset = start; *endOffset = end;
        return s.mid(start, end - start);
    }
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
    if (isRowBoundary(boundaryType)) {
        const int rowStart = rowAt(s, offset).first;
        if (rowStart == 0) { *startOffset = 0; *endOffset = 0; return {}; }
        const auto [start, end] = rowAt(s, rowStart - 1);
        *startOffset = start; *endOffset = end;
        return s.mid(start, end - start);
    }
    QTextBoundaryFinder f(toFinderType(boundaryType), s);
    f.setPosition(offset);
    // ANTS-6654 — on a boundary, the segment before ends right here.
    int end = f.isAtBoundary() ? offset : f.toPreviousBoundary();
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
    if (isRowBoundary(boundaryType)) {
        const int rowEnd = rowAt(s, offset).second;
        if (rowEnd >= s.size()) { *startOffset = s.size(); *endOffset = s.size(); return {}; }
        const auto [start, end] = rowAt(s, rowEnd);
        *startOffset = start; *endOffset = end;
        return s.mid(start, end - start);
    }
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
