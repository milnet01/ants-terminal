// Why this exists: RIS reconstructs TerminalGrid, which reverted the
// theme colours and the configured scrollback depth to the values
// compiled into the header. See spec.md.
//
// Drives processAction with an EscDispatch action, final char 'c', the
// same public entry osc_color_query uses for OSC.

#include "terminalgrid.h"
#include "vtparser.h"

#include <gtest/gtest.h>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

void ris(TerminalGrid &g) {
    VtAction a;
    a.type = VtAction::EscDispatch;
    a.finalChar = 'c';
    g.processAction(a);
}

void print(TerminalGrid &g, const QString &text) {
    for (const QChar &ch : text) {
        VtAction a;
        a.type = VtAction::Print;
        a.codepoint = ch.unicode();
        g.processAction(a);
    }
}

void newline(TerminalGrid &g) {
    VtAction a;
    a.type = VtAction::EscDispatch;
    a.finalChar = 'D';  // IND — index, i.e. line feed
    g.processAction(a);
}

}  // namespace

TEST(RisPreservesConfiguration, Inv1ScrollbackCapacitySurvives) {
    TerminalGrid g(kRows, kCols);
    g.setMaxScrollback(250000);
    ris(g);
    EXPECT_EQ(g.maxScrollback(), 250000)
        << "RIS reverted the scrollback cap to the figure compiled into "
           "the header, silently shrinking a depth the user configured";
}

TEST(RisPreservesConfiguration, Inv2ThemeForegroundSurvives) {
    TerminalGrid g(kRows, kCols);
    const QColor fg(0x11, 0x22, 0x33);
    g.setDefaultFg(fg);
    ris(g);
    EXPECT_EQ(g.defaultFg(), fg)
        << "RIS reverted the default foreground to the compiled-in colour, "
           "so a shell's `reset` changed the user's theme";
}

TEST(RisPreservesConfiguration, Inv3ThemeBackgroundSurvives) {
    TerminalGrid g(kRows, kCols);
    const QColor bg(0x44, 0x55, 0x66);
    g.setDefaultBg(bg);
    ris(g);
    EXPECT_EQ(g.defaultBg(), bg)
        << "RIS reverted the default background to the compiled-in colour. "
           "Cursor and selection colours live on TerminalWidget and survive, "
           "so the result is one theme's text on another theme's furniture";
}

TEST(RisPreservesConfiguration, Inv4ScrollbackContentsAreStillCleared) {
    TerminalGrid g(kRows, kCols);
    g.setMaxScrollback(250000);
    // Push past the screen so lines land in scrollback.
    for (int i = 0; i < kRows + 10; ++i) {
        print(g, QStringLiteral("line"));
        newline(g);
    }
    ASSERT_GT(g.scrollbackSize(), 0) << "precondition: nothing reached scrollback";
    ris(g);
    EXPECT_EQ(g.scrollbackSize(), 0)
        << "preserving the scrollback CAPACITY must not preserve its LINES";
}

TEST(RisPreservesConfiguration, Inv5CursorIsStillReset) {
    TerminalGrid g(kRows, kCols);
    g.setDefaultFg(QColor(0x11, 0x22, 0x33));
    print(g, QStringLiteral("hello"));
    newline(g);
    ASSERT_NE(g.cursorRow() * kCols + g.cursorCol(), 0)
        << "precondition: cursor never left the origin";
    ris(g);
    EXPECT_EQ(g.cursorRow(), 0);
    EXPECT_EQ(g.cursorCol(), 0)
        << "RIS must still reset terminal state, not just spare the config";
}
