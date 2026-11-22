// ANTS-5791 — a copy right after a select reuses the selection's text.
// Contract: tests/features/selection_text_cache/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPointF>

#include <string>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};

    Harness() {
        w.resize(640, 240);
        w.grid()->resize(4, 20);
        feed("hello");
        const QPointF at(2, 2);   // the first cell of the top row
        QMouseEvent ev(QEvent::MouseButtonDblClick, at, w.mapToGlobal(at),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&w, &ev);
    }
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }
};

}  // namespace

// INV-1
TEST(SelectionTextCache, UnchangedSelectionIsNotRebuilt) {
    Harness h;
    const QString first = h.w.selectedText();
    const QString second = h.w.selectedText();
    ASSERT_EQ(first, QStringLiteral("hello"));
    EXPECT_EQ(first.constData(), second.constData());
}

// INV-2
TEST(SelectionTextCache, GridChangeRebuildsIt) {
    Harness h;
    ASSERT_EQ(h.w.selectedText(), QStringLiteral("hello"));
    h.feed("\rjelly");
    EXPECT_EQ(h.w.selectedText(), QStringLiteral("jelly"));
}
