// ANTS-5029 — feature-conformance test; see spec.md. INV-1 and INV-2 drive
// TerminalGrid through VtParser, headless. INV-3 is a source scrape, because
// re-run needs a live TerminalWidget and a modal dialog.

#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

#include <string>

namespace {

void feed(TerminalGrid &grid, const QByteArray &bytes) {
    VtParser parser([&grid](const VtAction &act) {
        grid.processAction(act);
    });
    parser.feed(bytes.constData(), bytes.size());
}

// Text of one global line: scrollback index first, then screen row.
QString lineText(const TerminalGrid &grid, int globalLine) {
    QString out;
    const int sb = grid.scrollbackSize();
    if (globalLine < 0) return out;
    if (globalLine < sb) {
        for (const Cell &c : grid.scrollbackLine(globalLine))
            out += QChar(static_cast<char16_t>(c.codepoint ? c.codepoint : ' '));
    } else if (globalLine - sb < grid.rows()) {
        for (int col = 0; col < grid.cols(); ++col) {
            const uint32_t cp = grid.cellAt(globalLine - sb, col).codepoint;
            out += QChar(static_cast<char16_t>(cp ? cp : ' '));
        }
    }
    return out;
}

QByteArray newlines(int n) { return QByteArray("\r\n").repeated(n); }

// One finished command block: prompt, command, output, exit status.
const QByteArray kBlock =
    "\x1b]133;A\x07$ \x1b]133;B\x07" "echo marker-5029\r\n"
    "\x1b]133;C\x07" "marker-output\r\n" "\x1b]133;D;0\x07";

}  // namespace

// INV-1 — a region recorded after the scrollback is full still names its
// command line once more lines have been evicted.
TEST(Osc133RerunSafety, Inv1RegionFollowsEviction) {
    TerminalGrid grid(5, 40);
    grid.setMaxScrollback(1000);   // the clamp's floor
    feed(grid, newlines(1200));    // scrollback is now full
    ASSERT_EQ(grid.scrollbackSize(), 1000);
    feed(grid, kBlock);
    feed(grid, newlines(50));      // every newline now evicts one line

    ASSERT_FALSE(grid.promptRegions().empty());
    const PromptRegion &pr = grid.promptRegions().back();
    EXPECT_TRUE(lineText(grid, pr.endLine)
                    .contains(QStringLiteral("echo marker-5029")))
        << "endLine " << pr.endLine << " holds: "
        << lineText(grid, pr.endLine).toStdString();
}

// INV-2 — a region whose prompt line has been evicted is dropped.
TEST(Osc133RerunSafety, Inv2EvictedRegionIsDropped) {
    TerminalGrid grid(5, 40);
    grid.setMaxScrollback(1000);
    feed(grid, kBlock);
    ASSERT_EQ(grid.promptRegions().size(), 1u);
    feed(grid, newlines(1200));    // the block's lines are evicted
    EXPECT_TRUE(grid.promptRegions().empty())
        << "a stale region survived at startLine "
        << grid.promptRegions().front().startLine;
}

// INV-3 — an unsigned re-run confirms before it types anything.
TEST(Osc133RerunSafety, Inv3UnsignedRerunConfirms) {
    const std::string body = ants_test::stripComments(
        ants_test::slurpFunctionBody(SRC_TERMINALWIDGET_PATH,
                                     "void TerminalWidget::rerunCommandAt("));
    ASSERT_FALSE(body.empty()) << "rerunCommandAt not found";
    EXPECT_NE(body.find("osc133HmacEnforced()"), std::string::npos)
        << "re-run must check whether the markers are signed";
    EXPECT_NE(body.find("showSendConfirmation("), std::string::npos)
        << "an unsigned re-run must go through the confirmation dialog";
}
