// The pointer over the terminal text is one shape, the I-beam (ANTS-6853).
// Contract: tests/features/terminal_pointer_shape/spec.md.
// Why this exists: a new widget, and a tab with mouse reporting on, showed
// the default arrow while other tabs showed the I-beam.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPointF>
#include <QString>

#include <string>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

const char *shapeName(Qt::CursorShape s) {
    switch (s) {
    case Qt::IBeamCursor:       return "IBeamCursor";
    case Qt::ArrowCursor:       return "ArrowCursor";
    case Qt::PointingHandCursor: return "PointingHandCursor";
    default:                    return "another shape";
    }
}

struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};

    // Output as the program writes it, straight into the widget's grid.
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }

    // A plain pointer move to (x, y) in widget pixels: no buttons, no keys.
    void moveTo(int x, int y) {
        QMouseEvent move(QEvent::MouseMove, QPointF(x, y), w.mapToGlobal(QPointF(x, y)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&w, &move);
    }

    // A y inside screen row 2 (index 1), as neighbouring tests compute it.
    int row2Y() { return w.cellHeightForTest() * 3 / 2 + 4; }

    Qt::CursorShape shape() { return w.cursor().shape(); }

    bool reporting() {
        const TerminalGrid *g = w.grid();
        return g->mouseButtonMode() || g->mouseMotionMode() || g->mouseAnyMode();
    }
};

}  // namespace

// INV-1 — before any mouse event.
TEST(TerminalPointerShape, Inv1NewWidgetShowsIBeam) {
    Harness h;
    EXPECT_EQ(h.shape(), Qt::IBeamCursor)
        << "a new widget shows " << shapeName(h.shape()) << ", expected IBeamCursor";
}

// INV-2 — mouse reporting on, whichever mode.
TEST(TerminalPointerShape, Inv2ReportingModesKeepIBeam) {
    struct Case { const char *name; const char *bytes; };
    const Case cases[] = {
        {"?1000h click-only reporting", "\x1b[?1000h"},
        {"?1002h button-motion reporting", "\x1b[?1002h"},
        {"?1003h any-motion reporting", "\x1b[?1003h"},
    };
    for (const Case &c : cases) {
        Harness h;
        h.feed("$ hello\r\n");
        h.feed(c.bytes);
        ASSERT_TRUE(h.reporting()) << c.name << ": the grid did not enter a reporting mode";
        h.moveTo(40, h.row2Y());
        EXPECT_EQ(h.shape(), Qt::IBeamCursor)
            << c.name << ": after a plain move the pointer is " << shapeName(h.shape())
            << ", expected IBeamCursor";
        // A second move into another cell must not change it either.
        h.moveTo(200, h.row2Y());
        EXPECT_EQ(h.shape(), Qt::IBeamCursor)
            << c.name << ": after a second move the pointer is " << shapeName(h.shape())
            << ", expected IBeamCursor";
    }
}

// INV-3 — reporting off, plain text.
TEST(TerminalPointerShape, Inv3PlainTextWithReportingOffIsIBeam) {
    Harness h;
    h.feed("\x1b[2;1Hsee https://example.com");
    ASSERT_FALSE(h.reporting()) << "a new grid has mouse reporting on";
    h.moveTo(0, h.row2Y());   // column 0: the letter 's', not part of the URL
    EXPECT_EQ(h.shape(), Qt::IBeamCursor)
        << "over plain text with reporting off the pointer is " << shapeName(h.shape())
        << ", expected IBeamCursor";
}

// INV-4 — the hand over a URL, and not after reporting turns on.
TEST(TerminalPointerShape, Inv4HandOverUrlIsResetWhenReportingTurnsOn) {
    Harness h;
    h.feed("\x1b[2;1Hsee https://example.com");
    ASSERT_FALSE(h.reporting()) << "a new grid has mouse reporting on";
    const int y = h.row2Y();

    // Find a pixel on the URL by moving along the row; the cell width is
    // not exposed, and the widget clamps x into the grid.
    int urlX = -1;
    for (int x = 0; x < h.w.width() && urlX < 0; ++x) {
        h.moveTo(x, y);
        if (h.shape() == Qt::PointingHandCursor) urlX = x;
    }
    ASSERT_GE(urlX, 0) << "no pixel along row 2 gave the hand over 'https://example.com'; "
                       << "last pointer was " << shapeName(h.shape())
                       << ", expected PointingHandCursor";
    EXPECT_EQ(h.shape(), Qt::PointingHandCursor);

    h.feed("\x1b[?1003h");
    ASSERT_TRUE(h.reporting()) << "the grid did not enter a reporting mode";
    h.moveTo(urlX, y);
    EXPECT_EQ(h.shape(), Qt::IBeamCursor)
        << "over the same cell with reporting on the pointer is " << shapeName(h.shape())
        << ", expected IBeamCursor";
}
