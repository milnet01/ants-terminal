// A Sixel image decodes to the right pixels — see spec.md. ANTS-5076.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QImage>
#include <clocale>
#include <gtest/gtest.h>
#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

struct Harness {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

std::string sixel(const std::string &body) {
    return "\x1BPq" + body + "\x1B\\";
}

// Decodes `body` and returns the image, or a null QImage when none was made.
QImage decode(const std::string &body) {
    std::setlocale(LC_CTYPE, "");
    Harness h;
    h.feed(sixel(body));
    if (h.grid.inlineImages().empty()) return QImage();
    return h.grid.inlineImages().back().image;
}

constexpr QRgb kRed = 0xFFFF0000;
constexpr QRgb kBlue = 0xFF0000FF;

}  // namespace

// INV-1
TEST(SixelDecodePixels, FullSixelPaintsSixRows) {
    const QImage img = decode("#1;2;100;0;0~~");
    ASSERT_FALSE(img.isNull());
    ASSERT_GE(img.width(), 2);
    ASSERT_GE(img.height(), 6);
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 6; ++y)
            EXPECT_EQ(img.pixel(x, y), kRed) << "x=" << x << " y=" << y;
}

// INV-2
TEST(SixelDecodePixels, RepeatPaintsEveryColumn) {
    const QImage img = decode("#1;2;0;0;100!3~");
    ASSERT_FALSE(img.isNull());
    ASSERT_GE(img.width(), 3);
    ASSERT_GE(img.height(), 6);
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 6; ++y)
            EXPECT_EQ(img.pixel(x, y), kBlue) << "x=" << x << " y=" << y;
}

// INV-3
TEST(SixelDecodePixels, OnlySetBitsArePainted) {
    const QImage img = decode("#1;2;0;0;100@");
    ASSERT_FALSE(img.isNull());
    ASSERT_GE(img.height(), 2);
    EXPECT_EQ(img.pixel(0, 0), kBlue);
    EXPECT_EQ(qAlpha(img.pixel(0, 1)), 0);
}

// INV-4
TEST(SixelDecodePixels, ColourPassesKeepTheirColumns) {
    const QImage img = decode("#1;2;100;0;0~$#2;2;0;0;100?~");
    ASSERT_FALSE(img.isNull());
    ASSERT_GE(img.width(), 2);
    for (int y = 0; y < 6; ++y) {
        EXPECT_EQ(img.pixel(0, y), kRed) << "y=" << y;
        EXPECT_EQ(img.pixel(1, y), kBlue) << "y=" << y;
    }
}
