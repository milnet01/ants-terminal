// ANTS-6532 — a double-click goes to the app while it has mouse reporting on.
// Contract: tests/features/mouse_report_double_click/spec.md.

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
    }
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }

    // A double-click on the first cell of the top row.
    void doubleClick(Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const QPointF at(2, 2);
        QMouseEvent ev(QEvent::MouseButtonDblClick, at, w.mapToGlobal(at),
                       Qt::LeftButton, Qt::LeftButton, mods);
        QCoreApplication::sendEvent(&w, &ev);
    }
};

}  // namespace

// INV-1
TEST(MouseReportDoubleClick, ReportingOffSelectsWord) {
    Harness h;
    h.doubleClick();
    EXPECT_EQ(h.w.selectedText(), QStringLiteral("hello"));
}

// INV-2
TEST(MouseReportDoubleClick, ReportingOnSelectsNothing) {
    Harness h;
    h.feed("\x1B[?1000h");
    h.doubleClick();
    EXPECT_EQ(h.w.selectedText(), QString());
}

// INV-3
TEST(MouseReportDoubleClick, ShiftOverridesReporting) {
    Harness h;
    h.feed("\x1B[?1000h");
    h.doubleClick(Qt::ShiftModifier);
    EXPECT_EQ(h.w.selectedText(), QStringLiteral("hello"));
}
