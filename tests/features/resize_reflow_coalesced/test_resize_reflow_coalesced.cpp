// ANTS-3456 — a window drag re-sizes the grid at most twice, not once per
// step. Contract: spec.md beside this file.

#include "terminalgrid.h"
#include "terminalwidget.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QScrollBar>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

// Run the event loop for `ms`, so a pending settle timer can fire.
void spin(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// A shown host makes the terminal a visible child, so resize() sends the
// resize event straight away instead of at the next show.
struct Harness {
    QWidget host;
    TerminalWidget *w = nullptr;

    Harness() {
        host.resize(1400, 700);
        w = new TerminalWidget(&host);
        w->setGeometry(0, 0, 1000, 600);
        host.show();
        QCoreApplication::processEvents();
        settle();
    }
    // Past any settle window the show and the first layout opened.
    void settle() const { spin(3 * TerminalWidget::kResizeSettleMs); }
};

}  // namespace

TEST(ResizeReflowCoalesced, Inv1EventsInsideOneWindowDoNotReflow) {
    Harness h;
    const int before = h.w->grid()->cols();
    h.w->resize(900, 600);                 // leading edge: applies at once
    const int first = h.w->grid()->cols();
    ASSERT_NE(first, before) << "the first resize should apply at once";
    for (int i = 1; i < 10; ++i) {
        h.w->resize(900 - 40 * i, 600);
        EXPECT_EQ(h.w->grid()->cols(), first)
            << "resize " << i << " inside the settle window re-sized the grid";
    }
}

TEST(ResizeReflowCoalesced, Inv2LastSizeLandsAfterTheWindow) {
    Harness h;
    h.w->resize(900, 600);
    const int first = h.w->grid()->cols();
    h.w->resize(600, 500);
    h.settle();
    const int cols = h.w->grid()->cols();
    const int rows = h.w->grid()->rows();
    EXPECT_NE(cols, first) << "the last resize never landed";
    h.w->forceRecalcSize();
    EXPECT_EQ(h.w->grid()->cols(), cols);
    EXPECT_EQ(h.w->grid()->rows(), rows);
}

TEST(ResizeReflowCoalesced, Inv3OtherCallersApplyAtOnce) {
    Harness h;
    h.w->resize(900, 600);
    h.w->resize(860, 600);                 // now inside a settle window
    const int beforeFont = h.w->grid()->cols();
    h.w->setFontSize(h.w->fontSize() * 2);
    const int afterFont = h.w->grid()->cols();
    ASSERT_NE(afterFont, beforeFont) << "the font change should change the grid";
    h.w->forceRecalcSize();
    EXPECT_EQ(h.w->grid()->cols(), afterFont) << "setFontSize waited for the timer";
}

TEST(ResizeReflowCoalesced, Inv4ScrollBarFollowsADeferredResize) {
    Harness h;
    auto *bar = h.w->findChild<QScrollBar *>();
    ASSERT_NE(bar, nullptr);
    h.w->resize(900, 600);
    h.w->resize(700, 600);                 // deferred
    EXPECT_EQ(bar->geometry().right(), h.w->width() - 1);
}
