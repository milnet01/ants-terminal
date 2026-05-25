// Feature-conformance test for ANTS-1841 — see spec.md.
//
// Four indie-review #6 hot-path hygiene fixes on TerminalWidget. All are
// GUI / paint-path concerns that can't be driven headlessly, so each
// invariant is a source-scrape against terminalwidget.cpp (same pattern
// as scrollback_frozen_view). The assertions lock the post-fix shape so a
// future edit can't silently reintroduce the redundant work.

#include <QFile>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString readSource(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Brace-balanced body extraction starting at the first `{` after the
// signature. Mirrors the helper in scrollback_frozen_view.
QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int braceStart = src.indexOf(QChar('{'), start);
    if (braceStart < 0) return QString();
    int depth = 1;
    int i = braceStart + 1;
    while (i < src.size() && depth > 0) {
        const QChar c = src.at(i);
        if (c == QChar('{')) ++depth;
        else if (c == QChar('}')) --depth;
        ++i;
    }
    return src.mid(braceStart, i - braceStart);
}

QString tw() { return readSource(QStringLiteral(SRC_TERMINALWIDGET_PATH)); }

}  // namespace

// INV-1 — mouseMoveEvent resolves URL spans through the cache, not a raw
// per-pixel regex run.
TEST(TerminalWidgetHotPathPerf, Inv1HoverUsesSpanCache) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::mouseMoveEvent("));
    ASSERT_FALSE(body.isEmpty()) << "mouseMoveEvent not found";
    EXPECT_TRUE(body.contains(QStringLiteral("urlSpansForLine(")))
        << "mouseMoveEvent must hit the cached urlSpansForLine() front-end";
    EXPECT_FALSE(body.contains(QStringLiteral("detectUrls(")))
        << "mouseMoveEvent still calls detectUrls() directly — the URL "
           "regex runs on every pixel of motion";
}

// INV-2 — loadHistory dedups with a set, not a linear list scan.
TEST(TerminalWidgetHotPathPerf, Inv2HistoryDedupNotQuadratic) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::loadHistory()"));
    ASSERT_FALSE(body.isEmpty()) << "loadHistory not found";
    EXPECT_TRUE(body.contains(QStringLiteral("QSet<QString>")))
        << "loadHistory must dedup via a QSet (O(1) membership)";
    EXPECT_FALSE(body.contains(QStringLiteral("m_historyEntries.contains")))
        << "loadHistory still uses m_historyEntries.contains() — O(n^2) "
           "dedup over the whole shell history";
}

// INV-3 — the triple-click branch clears the rectangular-selection flag
// before its early return.
TEST(TerminalWidgetHotPathPerf, Inv3TripleClickClearsRectFlag) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::mousePressEvent("));
    ASSERT_FALSE(body.isEmpty()) << "mousePressEvent not found";
    const int lo = body.indexOf(QStringLiteral("Triple-click: select entire line"));
    ASSERT_GE(lo, 0) << "triple-click branch anchor missing";
    // The branch ends at its own bookkeeping reset; scope tightly so we
    // don't accidentally match the fall-through reset below it.
    const int hi = body.indexOf(QStringLiteral("m_clickCount = 0;"), lo);
    ASSERT_GT(hi, lo) << "triple-click branch end anchor missing";
    const QString branch = body.mid(lo, hi - lo);
    EXPECT_TRUE(branch.contains(QStringLiteral("m_rectSelection = false")))
        << "triple-click full-line selection does not declare linear mode; "
           "a prior Alt-drag leaves m_rectSelection set";
}

// INV-4 — paintEvent evaluates the search-match predicate at most once
// per cell.
TEST(TerminalWidgetHotPathPerf, Inv4SearchMatchComputedOncePerCell) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::paintEvent("));
    ASSERT_FALSE(body.isEmpty()) << "paintEvent not found";
    EXPECT_TRUE(body.contains(QStringLiteral("const bool searchMatch")))
        << "paintEvent must hoist the per-cell search-match predicate";
    EXPECT_EQ(body.count(QStringLiteral("isCellSearchMatch(globalLine, col)")), 1)
        << "isCellSearchMatch(globalLine, col) is evaluated more than once "
           "per painted cell — colour + opacity decisions must share one "
           "result";
}
