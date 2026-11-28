// ANTS-6654 — mouse reports: release, drag, extra buttons and the wheel.
// Contract: tests/features/mouse_report_encoding/spec.md.

#include <gtest/gtest.h>

#include <QByteArray>

#include "terminalwidget.h"

namespace {

// The normal (non-SGR) report for base code `cb`, at column 5, row 3.
QByteArray normal(int cb) {
    QByteArray seq("\x1B[M");
    seq.append(static_cast<char>(cb + 32));
    seq.append(static_cast<char>(5 + 32));
    seq.append(static_cast<char>(3 + 32));
    return seq;
}

QByteArray encode(int button, int mods, bool press, bool release, bool sgr) {
    return TerminalWidget::encodeMouseReport(button, 5, 3, mods, press, release, sgr);
}

}  // namespace

// INV-1
TEST(MouseReportEncoding, NormalFormReportsRelease) {
    EXPECT_EQ(encode(0, 0, false, true, false), normal(3));
    EXPECT_EQ(encode(2, 16, false, true, false), normal(3 + 16));  // Ctrl held
}

// INV-2
TEST(MouseReportEncoding, DragCarriesMotionBit) {
    EXPECT_EQ(encode(0, 0, false, false, false), normal(32));
    EXPECT_EQ(encode(0, 0, false, false, true), QByteArray("\x1B[<32;5;3M"));
    EXPECT_EQ(encode(35, 0, false, false, false), normal(35));
    EXPECT_EQ(encode(35, 0, false, false, true), QByteArray("\x1B[<35;5;3M"));
}

// INV-3
TEST(MouseReportEncoding, PressUnchanged) {
    EXPECT_EQ(encode(0, 0, true, false, false), normal(0));
    EXPECT_EQ(encode(0, 0, true, false, true), QByteArray("\x1B[<0;5;3M"));
    EXPECT_EQ(encode(0, 0, false, true, true), QByteArray("\x1B[<0;5;3m"));
    EXPECT_EQ(encode(64, 0, true, false, false), normal(64));  // wheel up
}

// INV-4
TEST(MouseReportEncoding, ExtraButtonsSendNothing) {
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::BackButton, Qt::BackButton, true, false), -1);
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::ForwardButton, Qt::NoButton, false, true), -1);
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::LeftButton,
                                                Qt::LeftButton | Qt::BackButton, true, false), 0);
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::RightButton, Qt::RightButton, true, false), 2);
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::NoButton, Qt::NoButton, false, false), 35);
    EXPECT_EQ(TerminalWidget::mouseReportButton(Qt::NoButton, Qt::MiddleButton, false, false), 1);
}

// INV-5
TEST(MouseReportEncoding, WheelReportsPerNotch) {
    int accum = 0;
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 30), 0);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 30), 0);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 30), 0);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 30), 1);
    EXPECT_EQ(accum, 0);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 240), 2);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, -120), -1);
    EXPECT_EQ(TerminalWidget::takeWheelNotches(accum, 150), 1);
    EXPECT_EQ(accum, 30);
}
