// ANTS-6702 — a cell stores its colours in 4 bytes each. Contract: spec.md
// beside this file.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>

#include <QColor>
#include <string>

TEST(CompactCellColor, Inv1CellIsSmall) {
    EXPECT_LE(sizeof(Cell), 28u) << "sizeof(Cell) = " << sizeof(Cell);
}

TEST(CompactCellColor, Inv2ColourRoundTrips) {
    for (const QColor &in : {QColor(0x12, 0x34, 0x56), QColor(255, 255, 255),
                             QColor(0, 0, 0), QColor(10, 20, 30, 128)}) {
        Cell c;
        c.attrs.fg = in;
        c.attrs.bg = in;
        c.attrs.underlineColor = in;
        const QColor fg = c.attrs.fg, bg = c.attrs.bg, ul = c.attrs.underlineColor;
        EXPECT_TRUE(fg.isValid() && bg.isValid() && ul.isValid());
        EXPECT_EQ(fg.rgba(), in.rgba());
        EXPECT_EQ(bg.rgba(), in.rgba());
        EXPECT_EQ(ul.rgba(), in.rgba());
    }
    // Every RGBA value is a colour, including the one CellColor uses inside
    // for "unset"; it may come back as another fully transparent value.
    for (QRgb v = 0x00000000u; v <= 0x00FFFFFFu; ++v) {
        Cell c;
        c.attrs.bg = QColor::fromRgba(v);
        if (!QColor(c.attrs.bg).isValid()) {
            ADD_FAILURE() << "transparent colour " << std::hex << v << " read back unset";
            break;
        }
    }
}

TEST(CompactCellColor, Inv3UnsetStaysInvalid) {
    Cell c;
    EXPECT_FALSE(QColor(c.attrs.underlineColor).isValid());
    c.attrs.underlineColor = QColor(1, 2, 3);
    c.attrs.underlineColor = QColor();
    EXPECT_FALSE(QColor(c.attrs.underlineColor).isValid());
    c.attrs.bg = QColor();
    EXPECT_FALSE(QColor(c.attrs.bg).isValid());
}

TEST(CompactCellColor, Inv4SgrColoursLandInTheCell) {
    TerminalGrid grid(5, 20);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });
    const std::string s = "\x1b[38;2;1;2;3m\x1b[48;2;4;5;6m\x1b[58;2;7;8;9mA\x1b[0mB";
    parser.feed(s.data(), static_cast<int>(s.size()));
    const Cell &a = grid.cellAt(0, 0);
    EXPECT_EQ(QColor(a.attrs.fg), QColor(1, 2, 3));
    EXPECT_EQ(QColor(a.attrs.bg), QColor(4, 5, 6));
    EXPECT_EQ(QColor(a.attrs.underlineColor), QColor(7, 8, 9));
    const Cell &b = grid.cellAt(0, 1);
    EXPECT_FALSE(QColor(b.attrs.underlineColor).isValid());
}
