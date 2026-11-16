// Why this exists: setDefaultFg/setDefaultBg skipped the saved main screen,
// scrollback and saved SGR state, so after a theme change plus alt-screen
// exit the text area kept the old theme's colours. See spec.md.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>

#include <string>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

const QColor kOldFg(0xAA, 0xBB, 0xCC);
const QColor kOldBg(0x10, 0x20, 0x30);
const QColor kNewFg(0x01, 0x02, 0x03);
const QColor kNewBg(0xF0, 0xE0, 0xD0);

struct Rig {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};

    Rig() {
        grid.setDefaultFg(kOldFg);
        grid.setDefaultBg(kOldBg);
    }
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
    void changeTheme() {
        grid.setDefaultFg(kNewFg);
        grid.setDefaultBg(kNewBg);
    }
    QColor fg(int r, int c) { return QColor(grid.cellAt(r, c).attrs.fg); }
    QColor bg(int r, int c) { return QColor(grid.cellAt(r, c).attrs.bg); }
};

}  // namespace

TEST(ThemeChangeRecoloursSavedScreen, Inv1SavedMainScreenTakesNewDefaults) {
    Rig r;
    r.feed("\x1b[HAB");
    r.feed("\x1b[?1049h");
    r.changeTheme();
    r.feed("\x1b[?1049l");
    EXPECT_EQ(r.fg(0, 0), kNewFg) << "text cell fg after alt-screen exit";
    EXPECT_EQ(r.bg(0, 0), kNewBg) << "text cell bg after alt-screen exit";
    EXPECT_EQ(r.bg(0, 40), kNewBg) << "blank cell bg after alt-screen exit";
    EXPECT_EQ(r.bg(kRows - 1, kCols - 1), kNewBg) << "last cell bg";
}

TEST(ThemeChangeRecoloursSavedScreen, Inv2ScrollbackTakesNewDefaults) {
    Rig r;
    for (int i = 0; i < kRows + 15; ++i)
        r.feed("line\r\n");
    ASSERT_GT(r.grid.scrollbackSize(), 0) << "precondition: no scrollback";
    r.changeTheme();
    for (int i = 0; i < r.grid.scrollbackSize(); ++i) {
        const auto &cells = r.grid.scrollbackLine(i);
        // Scrollback lines are stored trimmed to the text written, so only
        // check cells that exist.
        ASSERT_FALSE(cells.empty()) << "scrollback line " << i << " has no cells";
        const auto &last = cells.back();
        EXPECT_EQ(QColor(cells[0].attrs.fg), kNewFg) << "scrollback line " << i << " first cell fg";
        EXPECT_EQ(QColor(cells[0].attrs.bg), kNewBg) << "scrollback line " << i << " first cell bg";
        EXPECT_EQ(QColor(last.attrs.fg), kNewFg) << "scrollback line " << i << " last cell fg";
        EXPECT_EQ(QColor(last.attrs.bg), kNewBg) << "scrollback line " << i << " last cell bg";
    }
}

TEST(ThemeChangeRecoloursSavedScreen, Inv3Restored1049PenUsesNewDefaults) {
    Rig r;
    r.feed("\x1b[?1049h");
    r.changeTheme();
    r.feed("\x1b[?1049l");
    r.feed("\x1b[HQ");
    EXPECT_EQ(r.fg(0, 0), kNewFg) << "new text fg after 1049 exit";
    EXPECT_EQ(r.bg(0, 0), kNewBg) << "new text bg after 1049 exit";
}

TEST(ThemeChangeRecoloursSavedScreen, Inv4DecrcPenUsesNewDefaults) {
    Rig r;
    r.feed("\x1b[H\x1b" "7");
    r.changeTheme();
    r.feed("\x1b" "8Q");
    EXPECT_EQ(r.fg(0, 0), kNewFg) << "new text fg after DECRC";
    EXPECT_EQ(r.bg(0, 0), kNewBg) << "new text bg after DECRC";
}

TEST(ThemeChangeRecoloursSavedScreen, Inv5ExplicitColoursSurvive) {
    Rig r;
    r.feed("\x1b[H\x1b[31;44mX\x1b[0m");
    const QColor redFg = r.fg(0, 0);
    const QColor blueBg = r.bg(0, 0);
    ASSERT_NE(redFg, kOldFg) << "precondition: SGR 31 gave the default fg";
    ASSERT_NE(blueBg, kOldBg) << "precondition: SGR 44 gave the default bg";
    // Push line 0 into scrollback; leave a second copy on the saved screen.
    for (int i = 0; i < kRows + 5; ++i)
        r.feed("\r\n");
    r.feed("\x1b[H\x1b[31;44mX\x1b[0m");
    ASSERT_GT(r.grid.scrollbackSize(), 0) << "precondition: no scrollback";
    r.feed("\x1b[?1049h");
    r.changeTheme();
    r.feed("\x1b[?1049l");
    EXPECT_EQ(r.fg(0, 0), redFg) << "saved screen explicit fg";
    EXPECT_EQ(r.bg(0, 0), blueBg) << "saved screen explicit bg";
    const auto &cells = r.grid.scrollbackLine(0);
    ASSERT_FALSE(cells.empty()) << "scrollback line 0 has no cells";
    EXPECT_EQ(QColor(cells[0].attrs.fg), redFg) << "scrollback explicit fg";
    EXPECT_EQ(QColor(cells[0].attrs.bg), blueBg) << "scrollback explicit bg";
}
