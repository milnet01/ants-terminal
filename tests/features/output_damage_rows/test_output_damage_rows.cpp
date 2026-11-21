// Output redraws only the rows it changed (ANTS-3464).
// Contract: tests/features/output_damage_rows/spec.md.

#include <gtest/gtest.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegion>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"
#include "vtstream.h"

namespace {

struct Harness {
    TerminalWidget w;
    QImage canvas;

    Harness() {
        w.resize(480, 240);
        w.setMaxScrollback(1000);   // the smallest cap the grid accepts
        // Also delivers the pending resize event. The screen starts blank; a
        // clear here would keep the next scrolled rows out of scrollback for
        // a moment (TerminalGrid::scrollUp's doubling guard).
        canvas = fullPaint();
    }

    // Output as the widget receives it: one parsed batch through onVtBatch.
    void feed(const std::string &s, bool clearSelectionHint = false) {
        auto batch = std::make_shared<VtBatch>();
        batch->clearSelectionHint = clearSelectionHint;
        VtParser parser([&batch](const VtAction &a) { batch->actions.push_back(a); });
        parser.feed(s.data(), static_cast<int>(s.size()));
        batch->rawBytes = QByteArray(s.data(), static_cast<qsizetype>(s.size()));
        VtBatchPtr shipped = batch;
        ASSERT_TRUE(QMetaObject::invokeMethod(&w, "onVtBatch", Qt::DirectConnection,
                                              Q_ARG(VtBatchPtr, shipped)));
    }

    // The widget alone: child widgets (scroll bar, search bar) repaint on
    // their own, so they are outside what the output path owes.
    QImage fullPaint() {
        QImage img(w.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::black);
        w.render(&img, QPoint(), QRegion(w.rect()), QWidget::DrawWindowBackground);
        return img;
    }

    // Paint only the damage the output path issued onto the kept canvas,
    // as the window system would.
    QRegion repaintCanvas() {
        const QRegion damage = w.takeOutputDamageForTest();
        // render() puts the region's top-left at the target offset.
        if (!damage.isEmpty())
            w.render(&canvas, damage.boundingRect().topLeft(), damage,
                     QWidget::DrawWindowBackground);
        return damage;
    }

    // Covers the whole widget. QRegion::contains(QRect) only tests overlap.
    bool isFull(const QRegion &r) const { return (QRegion(w.rect()) - r).isEmpty(); }
};

// The first pixel row where the two pictures differ, or -1.
int firstDifferentRow(const QImage &a, const QImage &b) {
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) return y;
    return -1;
}

void expectSamePicture(Harness &h, const std::string &step) {
    h.repaintCanvas();
    const QImage truth = h.fullPaint();
    const int y = firstDifferentRow(h.canvas, truth);
    EXPECT_EQ(y, -1) << "after feeding " << step << ": the partially painted picture "
                     << "differs from a full paint from pixel row " << y
                     << " (text row " << y / h.w.cellHeightForTest() << ")";
}

}  // namespace

// INV-1 — every scenario, one batch at a time, matches a full paint.
TEST(OutputDamageRows, Inv1PartialPaintsMatchAFullPaint) {
    Harness h;
    QJsonObject rule;
    rule["pattern"] = QStringLiteral("ERROR");
    rule["action_type"] = QStringLiteral("highlight_line");
    rule["action_value"] = QStringLiteral("#ff0000/#330000");
    h.w.setTriggerRules(QJsonArray{rule});

    const std::vector<std::string> steps = {
        // typing at a prompt, with a backspace
        "$ ", "e", "c", "h", "o", "\b\x1b[K", "o", " hi",
        // cursor moves that write nothing
        "\x1b[3;5H", "\x1b[1;1H", "\x1b[6;20H", "\x1b[A", "\x1b[C", "\x1b[1;3H",
        // a spinner on one row, and a status row above it rewritten in place
        "\x1b[6;1H\xe2\xa0\x8b working", "\r\xe2\xa0\x99 working",
        "\x1b[2A\r\x1b[2Kstatus 1\x1b[2B", "\r\xe2\xa0\xb9 working",
        // wide and combining text, then erased
        "\r\n\xe4\xb8\xad\xe6\x96\x87 e\xcc\x81", "\b\b\x1b[K\xe5\xad\x97",
        // bracket matching as the cursor walks onto and off a bracket
        "\r\n\x1b[2Kif (a && (b)) {", "\x1b[3D", "\x1b[D", "\x1b[5D", "\x1b[4C",
        // ...and a pair on two rows: onto the closing brace, then off it
        "\r\n\x1b[2Kblock {", "\r\n\x1b[2K  body }", "\x1b[D", "\x1b[D",
        // a trigger rule highlighting a completed line
        "\r\nERROR here\r\n", "fine\r\n",
    };
    for (const std::string &s : steps) {
        h.feed(s);
        expectSamePicture(h, s);
    }
}

// INV-2 — one typed character damages no more than the cursor row's band.
TEST(OutputDamageRows, Inv2OneCharacterDamagesOneRow) {
    Harness h;
    h.feed("$ ");
    h.repaintCanvas();
    h.feed("x");
    const QRegion damage = h.repaintCanvas();
    ASSERT_FALSE(damage.isEmpty());
    EXPECT_FALSE(h.isFull(damage));
    EXPECT_LE(damage.boundingRect().height(), h.w.cellHeightForTest() + 8)
        << "one character repainted " << damage.boundingRect().height() << " px of "
        << h.w.height();
}

// INV-3 — each fallback repaints everything.
TEST(OutputDamageRows, Inv3FallbacksRepaintEverything) {
    struct Case {
        const char *name;
        std::function<void(Harness &)> setUp;
        std::string bytes;
        bool clearSelectionHint = false;
    };
    const std::vector<Case> cases = {
        {"F2 a sync block ends", [](Harness &h) { h.feed("\x1b[?2026hx"); h.repaintCanvas(); },
         "y\x1b[?2026l"},
        {"F3 output scrolls", nullptr, std::string(40, '\n')},
        {"F4 every row changes", nullptr, "\x1b[2J"},
        {"F5 the cursor hides", nullptr, "\x1b[?25l"},
        {"F5 the cursor changes shape", nullptr, "\x1b[4 q"},
        {"F6 an image is on screen",
         [](Harness &h) {
             // A 1x1 RGB Kitty image, transmitted and displayed.
             h.feed("\x1b_Ga=T,f=24,s=1,v=1;AAAA\x1b\\");
             ASSERT_FALSE(h.w.grid()->inlineImages().empty());
             h.repaintCanvas();
         },
         "x"},
        {"F7 a prompt mark arrives", nullptr, "\x1b]133;A\x07"},
        {"F8 the performance overlay is on",
         [](Harness &h) { h.w.setShowPerformanceOverlay(true); h.repaintCanvas(); }, "x"},
        {"F9 output clears a selection",
         [](Harness &h) {
             h.feed("\x1b[2;1Hhello");
             const int y = h.w.cellHeightForTest() * 3 / 2 + 4;   // inside row 2
             QMouseEvent click(QEvent::MouseButtonDblClick, QPointF(4, y),
                               h.w.mapToGlobal(QPointF(4, y)), Qt::LeftButton,
                               Qt::LeftButton, Qt::NoModifier);
             QApplication::sendEvent(&h.w, &click);
             ASSERT_FALSE(h.w.selectedText().isEmpty()) << "the double-click selected nothing";
             h.repaintCanvas();
         },
         "\x1b[5;1Hx", true},
        {"F1 the view is scrolled up",
         [](Harness &h) {
             std::string lines;
             for (int i = 0; i < 200; ++i) lines += "line " + std::to_string(i) + "\r\n";
             h.feed(lines);
             QKeyEvent key(QEvent::KeyPress, Qt::Key_PageUp, Qt::ShiftModifier);
             QApplication::sendEvent(&h.w, &key);
             ASSERT_GT(h.w.scrollOffset(), 0) << "Shift+PageUp did not scroll the view";
             h.repaintCanvas();
         },
         "x"},
    };
    for (const Case &c : cases) {
        Harness h;
        h.feed("$ ");
        h.repaintCanvas();
        if (c.setUp) c.setUp(h);
        h.repaintCanvas();
        h.feed(c.bytes, c.clearSelectionHint);
        EXPECT_TRUE(h.isFull(h.repaintCanvas())) << c.name;
    }
}

// INV-4 — damage waiting behind the pacer survives a paint that clears the
// grid's dirty bits.
TEST(OutputDamageRows, Inv4PendingDamageSurvivesABlinkPaint) {
    Harness h;
    h.feed("\x1b[1;1Habc");          // paints at once and arms the pacer
    h.repaintCanvas();
    h.feed("\x1b[5;1Hdef");          // the pacer is running: pending
    const int cell = h.w.cellHeightForTest();
    QImage scratch(h.w.size(), QImage::Format_ARGB32_Premultiplied);
    h.w.render(&scratch, QPoint(), QRegion(0, 0, 4, 4),  // a blink-sized paint
               QWidget::DrawWindowBackground);
    const QRegion pending = h.w.takeOutputDamageForTest();
    EXPECT_FALSE(h.isFull(pending));
    EXPECT_TRUE(pending.intersects(QRect(0, cell * 4, h.w.width(), cell)))
        << "row 5's damage was lost to the paint in between";
}
