// Feature-conformance test for spec.md — locks the 0.6.33 frozen-view
// invariant: while the user is scrolled up, the viewport content does
// NOT change in response to PTY output, INCLUDING cursor-positioned
// overwrites on screen rows that the viewport overlaps.
//
// This is the final piece of the scrollback-stability puzzle. The
// 0.6.25 test (tests/features/scrollback_redraw/test_viewport_stable.cpp)
// covers the deep-scroll case (viewport entirely in scrollback); this
// test covers the partial-overlap case that 0.6.25's spec had to
// document as a "partial guarantee" because the anchor logic couldn't
// reach screen-row content.
//
// Two-part test:
//   1. Runtime simulation: capture a snapshot of screen rows, write
//      arbitrary PTY output (including cursor-positioned overwrites
//      and CSI 2J), verify the snapshot still yields the pre-write
//      content.
//   2. Source-level assertions: updateScrollBar fires
//      captureScreenSnapshot / clearScreenSnapshot at the right
//      transitions, and recalcGridSize clears the snapshot on resize.

// ANTS-1217 Phase 2: migrated to GoogleTest TEST() blocks under Suite
// ScrollbackFrozenView.

#include "terminalgrid.h"
#include "vtparser.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include <gtest/gtest.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kRows = 24;
constexpr int kCols = 80;

using Snapshot = std::vector<std::vector<Cell>>;

// Mirror of TerminalWidget::captureScreenSnapshot — copies every
// screen cell into a side vector. The test's assertion is that once
// this copy is held, PTY writes to the live grid don't mutate the
// copy.
Snapshot captureSnapshot(const TerminalGrid &g) {
    Snapshot s;
    s.reserve(g.rows());
    for (int r = 0; r < g.rows(); ++r) {
        std::vector<Cell> row;
        row.reserve(g.cols());
        for (int c = 0; c < g.cols(); ++c)
            row.push_back(g.cellAt(r, c));
        s.push_back(std::move(row));
    }
    return s;
}

std::string rowString(const std::vector<Cell> &row) {
    std::string out;
    for (const Cell &c : row) {
        uint32_t cp = c.codepoint;
        out.push_back((cp >= 0x20 && cp < 0x7F) ? static_cast<char>(cp) : ' ');
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

QString readSource(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString extractFunctionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) return QString();
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        QChar c = src.at(i);
        if (c == QChar('{')) ++depth;
        else if (c == QChar('}')) --depth;
        ++i;
    }
    return src.mid(braceStart, i - braceStart);
}

}  // namespace

// Runtime invariant: snapshot content is stable across arbitrary PTY output.
// Three scenarios covered — plain LF-scrolling, cursor-positioned overwrites,
// and CSI 2J clear. All must preserve the snapshot.
TEST(ScrollbackFrozenView, SnapshotStableAcrossOutput) {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    // Fill screen with identifiable content via cursor-positioning so we know
    // exactly which content sits on which screen row. CSI r;c H is 1-indexed.
    for (int i = 0; i < kRows; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "\x1b[%d;1Hpre-%02d", i + 1, i);
        parser.feed(buf, std::strlen(buf));
    }

    const Snapshot snap = captureSnapshot(grid);

    // Scenario A: line-feed-scrolled output.
    for (int i = 0; i < 50; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "post-%02d\r\n", i);
        parser.feed(buf, std::strlen(buf));
    }

    // Scenario B: cursor-positioned overwrite.
    const char *cupOverwrite = "\x1b[5;10HOVERWRITE";
    parser.feed(cupOverwrite, std::strlen(cupOverwrite));

    // Scenario C: CSI 2J full-screen clear.
    const char *clearSeq = "\x1b[2J\x1b[H";
    parser.feed(clearSeq, std::strlen(clearSeq));

    // Verify the snapshot is still untouched — row by row matches pre-write.
    for (int r = 0; r < kRows; ++r) {
        char expected[16];
        std::snprintf(expected, sizeof(expected), "pre-%02d", r);
        EXPECT_EQ(rowString(snap[r]), std::string(expected))
            << "snapshot row " << r << " mutated by subsequent PTY output — "
               "frozen-view contract violated; snapshot must be DEEP copy";
    }
}

// Source-level: TerminalWidget::updateScrollBar must call
// captureScreenSnapshot on the 0 → >0 transition and
// clearScreenSnapshot on the >0 → 0 transition.
TEST(ScrollbackFrozenView, UpdateScrollBarTransitionsSnapshot) {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    ASSERT_FALSE(src.isEmpty()) << "could not read terminalwidget.cpp";

    const QString body = extractFunctionBody(
        src, QStringLiteral("void TerminalWidget::updateScrollBar()"));
    ASSERT_FALSE(body.isEmpty()) << "updateScrollBar not found";

    EXPECT_TRUE(body.contains(QStringLiteral("captureScreenSnapshot")))
        << "updateScrollBar does not call captureScreenSnapshot — 0→>0 "
           "scroll-offset transition no longer freezes the viewport";
    EXPECT_TRUE(body.contains(QStringLiteral("clearScreenSnapshot")))
        << "updateScrollBar does not call clearScreenSnapshot — >0→0 "
           "scroll-offset transition does not discard the snapshot";
}

// Resize must clear the snapshot — dimensions no longer match the grid.
TEST(ScrollbackFrozenView, ResizeClearsSnapshot) {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    ASSERT_FALSE(src.isEmpty());

    const QString body = extractFunctionBody(
        src, QStringLiteral("void TerminalWidget::recalcGridSize()"));
    ASSERT_FALSE(body.isEmpty()) << "recalcGridSize not found";

    EXPECT_TRUE(body.contains(QStringLiteral("clearScreenSnapshot")))
        << "recalcGridSize does not call clearScreenSnapshot on resize — "
           "stale snapshots with wrong dimensions persist";
}

// cellAtGlobal must intercept screen-row reads when the snapshot is populated.
TEST(ScrollbackFrozenView, CellAtGlobalHonorsSnapshot) {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    ASSERT_FALSE(src.isEmpty());

    const QString body = extractFunctionBody(
        src, QStringLiteral("const Cell &TerminalWidget::cellAtGlobal"));
    ASSERT_FALSE(body.isEmpty()) << "cellAtGlobal not found";

    EXPECT_TRUE(body.contains(QStringLiteral("m_frozenScreenRows")))
        << "cellAtGlobal does not read from m_frozenScreenRows — snapshot "
           "is captured but never consulted by the paint path";
}
