#pragma once

#include <QAccessible>
#include <QAccessibleWidget>

class TerminalWidget;

// ANTS-1078 — QAccessibleInterface adapter that exposes a TerminalWidget's
// visible viewport to assistive technology (Orca, etc.) as readable text
// with a caret. Thin glue: all content logic lives in TerminalWidget's
// accessible*() helpers; this class holds NO per-widget content cache.
// See docs/specs/ANTS-1078.md.
class TerminalWidgetAccessible : public QAccessibleWidget,
                                 public QAccessibleTextInterface {
public:
    explicit TerminalWidgetAccessible(QWidget *w);

    void *interface_cast(QAccessible::InterfaceType t) override;

    using QAccessibleWidget::text;   // keep base text(QAccessible::Text) visible

    // QAccessibleTextInterface
    QString text(int startOffset, int endOffset) const override;
    int characterCount() const override;
    int cursorPosition() const override;
    void setCursorPosition(int) override {}                     // read-only in v1
    QString textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                         int *startOffset, int *endOffset) const override;
    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                             int *startOffset, int *endOffset) const override;
    QString textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                            int *startOffset, int *endOffset) const override;
    QRect characterRect(int offset) const override;
    int offsetAtPoint(const QPoint &point) const override;
    int selectionCount() const override { return 0; }          // v1: no AT selection
    void selection(int selectionIndex, int *startOffset, int *endOffset) const override;
    void addSelection(int, int) override {}
    void removeSelection(int) override {}
    void setSelection(int, int, int) override {}
    void scrollToSubstring(int, int) override {}
    QString attributes(int offset, int *startOffset, int *endOffset) const override;

private:
    TerminalWidget *term() const;
};

// Register the QAccessible factory so every TerminalWidget gets the adapter.
// Call once after the QApplication is constructed (e.g. in main()).
void installTerminalAccessibilityFactory();
