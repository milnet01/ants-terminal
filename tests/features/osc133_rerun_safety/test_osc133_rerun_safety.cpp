// ANTS-5029 — feature-conformance test; see spec.md. INV-1, INV-2 and INV-4
// drive TerminalGrid through VtParser, headless. INV-3 and INV-5 are source
// scrapes, because re-run and the context menu need a live TerminalWidget.

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

// INV-4 (ANTS-5078) — each region has a unique id. Looking an id up finds its
// region after the cap drops one in front of it, and finds nothing once the
// region itself is dropped.
TEST(Osc133RerunSafety, Inv4RegionIdSurvivesShift) {
    TerminalGrid grid(5, 40);
    feed(grid, kBlock);
    feed(grid, kBlock);
    ASSERT_EQ(grid.promptRegions().size(), 2u);
    const quint64 firstId = grid.promptRegions()[0].id;
    const quint64 secondId = grid.promptRegions()[1].id;
    EXPECT_NE(firstId, 0u) << "a region has no id";
    EXPECT_NE(firstId, secondId) << "two regions share an id";
    EXPECT_EQ(grid.promptRegionIndexById(secondId), 1);

    // Add prompts until the cap starts dropping the oldest region.
    for (int i = 0; i < 5000; ++i) {
        const std::size_t before = grid.promptRegions().size();
        feed(grid, QByteArray("\x1b]133;A\x07"));
        if (grid.promptRegions().size() == before) break;
    }
    EXPECT_EQ(grid.promptRegionIndexById(firstId), -1)
        << "a dropped region was still found";
    const int idx = grid.promptRegionIndexById(secondId);
    ASSERT_EQ(idx, 0) << "the surviving region was not found at its new index";
    EXPECT_EQ(grid.promptRegions()[idx].id, secondId);
}

// INV-5 (ANTS-5078) — the context menu's block actions name the block by id,
// never by an index taken before menu.exec() runs its event loop.
TEST(Osc133RerunSafety, Inv5ContextMenuCapturesBlockId) {
    const std::string body = ants_test::stripComments(
        ants_test::slurpFunctionBody(SRC_TERMINALWIDGET_PATH,
                                     "void TerminalWidget::contextMenuEvent("));
    ASSERT_FALSE(body.empty()) << "contextMenuEvent not found";
    // ", blockIdx]" is a lambda capture such as [this, blockIdx]; the
    // promptRegions()[blockIdx] read that labels the menu is not one.
    EXPECT_EQ(body.find(", blockIdx]"), std::string::npos)
        << "a menu action still captures the block index";
    EXPECT_NE(body.find("promptRegionIndexById("), std::string::npos)
        << "menu actions must look their block up by id when they run";
}
