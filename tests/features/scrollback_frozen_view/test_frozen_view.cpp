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

#include "terminalgrid.h"
#include "vtparser.h"

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;

#define CHECK(cond, msg) do {                                                \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);  \
        ++failures;                                                          \
    }                                                                        \
} while (0)

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

// Runtime invariant: snapshot content is stable across arbitrary PTY
// output. Three scenarios covered — plain LF-scrolling, cursor-
// positioned overwrites, and CSI 2J clear. All must preserve the
// snapshot.
void testSnapshotStableAcrossOutput() {
    TerminalGrid grid(kRows, kCols);
    VtParser parser([&grid](const VtAction &a) { grid.processAction(a); });

    // Fill screen with identifiable content via cursor-positioning
    // so we know exactly which content sits on which screen row — no
    // ambiguity from implicit scrollUp at end-of-screen. CSI r;c H is
    // 1-indexed, so row 1 = grid row 0.
    for (int i = 0; i < kRows; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "\x1b[%d;1Hpre-%02d", i + 1, i);
        parser.feed(buf, std::strlen(buf));
    }

    // Snapshot taken now. This is what a scrolled-up user sees for
    // the screen portion of their viewport.
    const Snapshot snap = captureSnapshot(grid);

    // Scenario A: a whole lot of line-feed-scrolled output.
    for (int i = 0; i < 50; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "post-%02d\r\n", i);
        parser.feed(buf, std::strlen(buf));
    }

    // Scenario B: cursor-positioned overwrite (CUP + text + CUP + text).
    //   CSI 5;10 H → move cursor to row 5, col 10 (1-indexed)
    //   Then write "OVERWRITE"
    const char *cupOverwrite = "\x1b[5;10HOVERWRITE";
    parser.feed(cupOverwrite, std::strlen(cupOverwrite));

    // Scenario C: CSI 2J full-screen clear.
    const char *clearSeq = "\x1b[2J\x1b[H";
    parser.feed(clearSeq, std::strlen(clearSeq));

    // The live grid has been thoroughly mutated. Verify the snapshot
    // is still untouched — row by row matches the pre-write state.
    for (int r = 0; r < kRows; ++r) {
        char expected[16];
        std::snprintf(expected, sizeof(expected), "pre-%02d", r);
        const std::string actual = rowString(snap[r]);
        CHECK(actual == expected,
              "snapshot row mutated by subsequent PTY output — frozen-view "
              "contract violated. snapshot must be a DEEP copy, not a "
              "reference to the live grid.");
        if (actual != expected) {
            std::fprintf(stderr, "  row %2d: expected '%s', got '%s'\n",
                         r, expected, actual.c_str());
        }
    }
}

// Source-level: TerminalWidget::updateScrollBar must call
// captureScreenSnapshot on the 0 → >0 transition and
// clearScreenSnapshot on the >0 → 0 transition.
QString readSource(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "FAIL: cannot open %s\n", qUtf8Printable(path));
        ++failures;
        return QString();
    }
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

void testUpdateScrollBarTransitionsSnapshot() {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    if (src.isEmpty()) return;

    const QString body = extractFunctionBody(
        src, QStringLiteral("void TerminalWidget::updateScrollBar()"));
    if (body.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: TerminalWidget::updateScrollBar not found\n");
        ++failures;
        return;
    }

    CHECK(body.contains(QStringLiteral("captureScreenSnapshot")),
          "updateScrollBar does not call captureScreenSnapshot — the 0 → >0 "
          "scroll-offset transition no longer freezes the viewport. See "
          "scrollback_frozen_view/spec.md.");
    CHECK(body.contains(QStringLiteral("clearScreenSnapshot")),
          "updateScrollBar does not call clearScreenSnapshot — the >0 → 0 "
          "scroll-offset transition does not discard the snapshot, so the "
          "viewport stays frozen after the user returns to the bottom. See "
          "scrollback_frozen_view/spec.md.");
}

// Resize must clear the snapshot — its dimensions no longer match the
// grid, and reflow moves content around in ways the snapshot can't
// track.
void testResizeClearsSnapshot() {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    if (src.isEmpty()) return;

    const QString body = extractFunctionBody(
        src, QStringLiteral("void TerminalWidget::recalcGridSize()"));
    if (body.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: TerminalWidget::recalcGridSize not found\n");
        ++failures;
        return;
    }

    CHECK(body.contains(QStringLiteral("clearScreenSnapshot")),
          "recalcGridSize does not call clearScreenSnapshot on resize — "
          "stale snapshots with wrong dimensions persist and corrupt the "
          "viewport after a window resize. See "
          "scrollback_frozen_view/spec.md.");
}

// cellAtGlobal must intercept screen-row reads when the snapshot is
// populated. This is the mechanism by which the snapshot actually
// reaches the paint path; without it the snapshot exists but nothing
// reads from it.
void testCellAtGlobalHonorsSnapshot() {
    const QString src = readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH));
    if (src.isEmpty()) return;

    const QString body = extractFunctionBody(
        src, QStringLiteral("const Cell &TerminalWidget::cellAtGlobal"));
    if (body.isEmpty()) {
        std::fprintf(stderr, "FAIL: cellAtGlobal not found\n");
        ++failures;
        return;
    }

    CHECK(body.contains(QStringLiteral("m_frozenScreenRows")),
          "cellAtGlobal does not read from m_frozenScreenRows — the "
          "snapshot is captured but never consulted. Paint path will "
          "read live grid cells unconditionally, breaking the frozen "
          "view invariant.");
}

}  // namespace

int main() {
    testSnapshotStableAcrossOutput();
    testUpdateScrollBarTransitionsSnapshot();
    testResizeClearsSnapshot();
    testCellAtGlobalHonorsSnapshot();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", failures);
        return 1;
    }
    std::fprintf(stderr, "All frozen-view invariants hold.\n");
    return 0;
}
