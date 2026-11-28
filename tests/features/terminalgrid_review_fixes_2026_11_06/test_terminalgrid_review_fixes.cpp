// Feature test: the smaller terminal-grid fixes from the 2026-11-06 reviews
// (ANTS-6653). See spec.md. Drives TerminalGrid through VtParser, or
// processAction where the parser cannot produce the action.

#include "inlineimagedecode.h"
#include "terminalgrid.h"
#include "vtparser.h"
#include "../../_support/xdg_guard.h"

#include <QBuffer>
#include <QByteArray>
#include <QImage>
#include <QTemporaryDir>

#include <clocale>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

struct Term {
    explicit Term(int rows = 24, int cols = 80) : grid(rows, cols) {
        grid.setOsc133KeyForTest(QByteArray());  // verifier off, whatever the host has
        grid.setResponseCallback([this](const std::string &r) { replies.push_back(r); });
    }
    void feed(const std::string &bytes) {
        parser.feed(bytes.data(), static_cast<int>(bytes.size()));
    }
    TerminalGrid grid;
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    std::vector<std::string> replies;
};

QByteArray imageBytes(int w, int h, const char *format) {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(qRgb(40, 120, 200));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, format);
    return bytes;
}

std::string pngBase64(int w, int h) {
    return imageBytes(w, h, "PNG").toBase64().toStdString();
}

std::string apc(const std::string &keys, const std::string &data = "") {
    return "\x1b_G" + keys + ";" + data + "\x1b\\";
}

void ris(TerminalGrid &g) {
    VtAction a;
    a.type = VtAction::EscDispatch;
    a.finalChar = 'c';
    g.processAction(a);
}

}  // namespace

// INV-1 — a CSI with a private marker is not its marker-free namesake.
TEST(TerminalGridReviewFixes, Inv1MarkedCsiIsNotItsNamesake) {
    Term t;
    t.feed("A");
    t.feed("\x1b[?1;1;0S");   // XTSMGRAPHICS, not SU
    t.feed("\x1b[?5;10r");    // XTRESTORE, not DECSTBM (which homes the cursor)
    EXPECT_EQ(t.grid.cellAt(0, 0).codepoint, uint32_t{'A'}) << "CSI ? S scrolled the screen";
    EXPECT_EQ(t.grid.cursorCol(), 1) << "CSI ? r was taken as DECSTBM";
    t.feed("\x1b[?2J");       // DECSED still erases
    EXPECT_EQ(t.grid.cellAt(0, 0).codepoint, uint32_t{' '});
}

// INV-2 — XTGETTCAP (`DCS + q … ST`) is not decoded as Sixel.
TEST(TerminalGridReviewFixes, Inv2XtgettcapIsNotSixel) {
    Term t;
    t.feed("\x1bP+q4d73\x1b\\");
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "XTGETTCAP placed a Sixel image";
}

// INV-3 — Kitty replies only to a command naming its image, and honours q=.
TEST(TerminalGridReviewFixes, Inv3KittyRepliesFollowIAndQ) {
    Term t;
    t.feed(apc("a=q"));
    EXPECT_TRUE(t.replies.empty()) << "a query with no i= was answered";
    t.feed(apc("a=q,i=5,q=1"));
    EXPECT_TRUE(t.replies.empty()) << "q=1 did not silence the OK";
    t.feed(apc("a=q,i=5"));
    ASSERT_EQ(t.replies.size(), 1u);
    EXPECT_EQ(t.replies.back(), "\x1b_Gi=5;OK\x1b\\");
    const std::string junk = QByteArray("not an image").toBase64().toStdString();
    t.feed(apc("a=T,f=100,i=6,q=1", junk));
    ASSERT_EQ(t.replies.size(), 2u) << "q=1 silenced an error";
    EXPECT_EQ(t.replies.back(), "\x1b_Gi=6;ENODATA\x1b\\");
    t.feed(apc("a=T,f=100,i=6,q=2", junk));
    EXPECT_EQ(t.replies.size(), 2u) << "q=2 did not silence the error";
}

// INV-4 — entering the alternate screen leaves no wide or wrap flags behind.
TEST(TerminalGridReviewFixes, Inv4AltScreenEntryClearsCellFlags) {
    std::setlocale(LC_CTYPE, "");
    Term t(24, 10);
    t.feed("\xe4\xbd\xa0" "abcdefghij");  // a wide char, then a wrap
    ASSERT_TRUE(t.grid.cellAt(0, 0).isWideChar) << "setup: no wide char";
    ASSERT_TRUE(t.grid.screenLineWrapped(0)) << "setup: no soft wrap";
    t.feed("\x1b[?1049h");
    EXPECT_FALSE(t.grid.cellAt(0, 0).isWideChar);
    EXPECT_FALSE(t.grid.cellAt(0, 1).isWideCont);
    EXPECT_FALSE(t.grid.screenLineWrapped(0));
}

// INV-5 — a narrowing resize drops blank bottom rows before content.
TEST(TerminalGridReviewFixes, Inv5ReflowDropsBlankRowsFirst) {
    Term t(10, 20);
    t.feed("0123456789abcdefghi\r\n$ ");
    t.grid.resize(5, 10);
    EXPECT_EQ(t.grid.scrollbackSize(), 0) << "content went to scrollback ahead of blank rows";
    EXPECT_EQ(t.grid.cellAt(0, 0).codepoint, uint32_t{'0'});
    EXPECT_EQ(t.grid.cellAt(2, 0).codepoint, uint32_t{'$'});
}

// INV-6 — DCH and ICH move OSC 8 links with their text; DCH breaks a wide pair.
TEST(TerminalGridReviewFixes, Inv6CharEditsMoveLinksAndBreakPairs) {
    {
        Term t;
        t.feed("ab\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\");
        ASSERT_EQ(t.grid.screenHyperlinks(0).size(), 1u);
        t.feed("\x1b[1;1H\x1b[1P");   // DCH 1 at column 0
        ASSERT_EQ(t.grid.screenHyperlinks(0).size(), 1u);
        EXPECT_EQ(t.grid.screenHyperlinks(0)[0].startCol, 1);
        EXPECT_EQ(t.grid.screenHyperlinks(0)[0].endCol, 4);
        t.feed("\x1b[2@");            // ICH 2 at column 0
        ASSERT_EQ(t.grid.screenHyperlinks(0).size(), 1u);
        EXPECT_EQ(t.grid.screenHyperlinks(0)[0].startCol, 3);
        EXPECT_EQ(t.grid.screenHyperlinks(0)[0].endCol, 6);
    }
    {
        std::setlocale(LC_CTYPE, "");
        Term t;
        t.feed("a\xe4\xbd\xa0" "b");
        ASSERT_TRUE(t.grid.cellAt(0, 1).isWideChar) << "setup: no wide char";
        t.feed("\x1b[1;3H\x1b[1P");   // delete the continuation half
        EXPECT_FALSE(t.grid.cellAt(0, 1).isWideChar) << "the lead kept its wide flag";
    }
}

// INV-7 — a truncated OSC 1337 image is refused.
TEST(TerminalGridReviewFixes, Inv7TruncatedOsc1337IsRefused) {
    TerminalGrid grid(24, 80);
    VtAction a;
    a.type = VtAction::OscEnd;
    a.payload = "1337;File=inline=1:" + pngBase64(16, 16);
    a.truncated = true;
    grid.processAction(a);
    EXPECT_TRUE(grid.inlineImages().empty());
}

// INV-8 — command output starting on line 0 is read from line 0.
TEST(TerminalGridReviewFixes, Inv8OutputAtLineZero) {
    Term t;
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07\x1b]133;C\x07");
    ASSERT_EQ(t.grid.promptRegions().size(), 1u);
    EXPECT_EQ(t.grid.promptRegionOutputLines(0).first, 0);
}

// INV-9 — Kitty delete: lowercase removes placements, uppercase frees data.
TEST(TerminalGridReviewFixes, Inv9KittyDeleteSelectors) {
    Term t;
    const std::string png = pngBase64(4, 4);
    t.feed(apc("a=T,f=100,i=7,q=2", png));
    ASSERT_EQ(t.grid.inlineImages().size(), 1u);
    t.feed(apc("a=d,d=i,i=7,q=2"));
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "d=i left the placement";
    t.feed(apc("a=p,i=7,q=2"));
    EXPECT_EQ(t.grid.inlineImages().size(), 1u) << "d=i freed the image data";
    t.feed(apc("a=d,d=I,i=7,q=2"));
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "d=I left the placement";
    t.feed(apc("a=p,i=7,q=2"));
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "d=I kept the image data";
    t.feed(apc("a=T,f=100,i=8,q=2", png));
    t.feed(apc("a=d,d=A,q=2"));
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "d=A did nothing";
    t.feed(apc("a=p,i=8,q=2"));
    EXPECT_TRUE(t.grid.inlineImages().empty()) << "d=A kept the image data";
}

// INV-10 — each screen keeps its own Kitty keyboard flag stack.
TEST(TerminalGridReviewFixes, Inv10KittyKeyboardStackPerScreen) {
    Term t;
    t.feed("\x1b[>1u");
    ASSERT_EQ(t.grid.kittyKeyFlags(), 1);
    t.feed("\x1b[?1049h");
    EXPECT_EQ(t.grid.kittyKeyFlags(), 0) << "the alternate screen took the main stack";
    t.feed("\x1b[>3u");
    t.feed("\x1b[?1049l");
    EXPECT_EQ(t.grid.kittyKeyFlags(), 1) << "the main screen's flags were lost";
    t.feed("\x1b[<u");
    EXPECT_EQ(t.grid.kittyKeyFlags(), 0);
}

// INV-11 — ConEmu OSC 9;N subcommands are not desktop notifications.
TEST(TerminalGridReviewFixes, Inv11ConEmuSubcommandsAreNotNotifications) {
    Term t;
    int fired = 0;
    t.grid.setNotifyCallback([&fired](const QString &, const QString &) { ++fired; });
    t.feed("\x1b]9;9;/home/someone\x1b\\");
    EXPECT_EQ(fired, 0) << "ConEmu's cwd report became a notification";
    t.feed("\x1b]9;42\x1b\\");
    EXPECT_EQ(fired, 1) << "a numeric message stopped notifying";
}

// INV-12 — an iTerm2 width that is not a bare cell count keeps the default.
TEST(TerminalGridReviewFixes, Inv12ITerm2WidthUnits) {
    Term t;
    t.feed("\x1b]1337;File=inline=1;width=100px:" + pngBase64(16, 16) + "\x07");
    ASSERT_EQ(t.grid.inlineImages().size(), 1u);
    EXPECT_EQ(t.grid.inlineImages().back().cellWidth, 2) << "100px read as 100 cells";
    t.feed("\x1b]1337;File=inline=1;width=5:" + pngBase64(16, 16) + "\x07");
    ASSERT_EQ(t.grid.inlineImages().size(), 2u);
    EXPECT_EQ(t.grid.inlineImages().back().cellWidth, 5);
}

// INV-13 — CPR reports the row relative to the scroll region under DECOM.
TEST(TerminalGridReviewFixes, Inv13CprHonoursOriginMode) {
    Term t;
    t.feed("\x1b[5;20r\x1b[?6h\x1b[2;1H\x1b[6n");
    ASSERT_EQ(t.replies.size(), 1u);
    EXPECT_EQ(t.replies.back(), "\x1b[2;1R");
}

// INV-14 — RIS keeps the SGR debug log on and the scrollback-insert pause.
TEST(TerminalGridReviewFixes, Inv14RisKeepsTerminalState) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ants_test::XdgGuard env;
    env.setEnv("XDG_DATA_HOME", dir.path().toLocal8Bit());
    TerminalGrid grid(24, 80);
    grid.setDebugLog(true);
    grid.setScrollbackInsertPaused(true);
    ris(grid);
    EXPECT_TRUE(grid.debugLog()) << "RIS turned the debug log off";
    EXPECT_TRUE(grid.scrollbackInsertPaused()) << "RIS cleared the scroll-up pause";
    grid.setDebugLog(false);
}

// INV-15 — an untyped inline image decodes only as PNG, JPEG or GIF.
TEST(TerminalGridReviewFixes, Inv15UntypedDecodeAllowlist) {
    EXPECT_FALSE(inlineimage::decodeBytes(imageBytes(4, 4, "PNG"), nullptr, 4096).isNull());
    const QByteArray jpeg = imageBytes(4, 4, "JPEG");  // empty without the JPEG plugin
    if (!jpeg.isEmpty())
        EXPECT_FALSE(inlineimage::decodeBytes(jpeg, nullptr, 4096).isNull());
    const QByteArray bmp = imageBytes(4, 4, "BMP");
    ASSERT_FALSE(bmp.isEmpty()) << "setup: no BMP writer";
    EXPECT_TRUE(inlineimage::decodeBytes(bmp, nullptr, 4096).isNull())
        << "a BMP reached Qt's format guessing";
}
