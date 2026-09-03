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

// INV-4 — paintEvent precomputes search-match spans per row (ANTS-3457)
// and evaluates a single shared per-cell predicate (ANTS-1841).
TEST(TerminalWidgetHotPathPerf, Inv4SearchMatchComputedOncePerCell) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::paintEvent("));
    ASSERT_FALSE(body.isEmpty()) << "paintEvent not found";
    EXPECT_TRUE(body.contains(QStringLiteral("const bool searchMatch")))
        << "paintEvent must hoist the per-cell search-match result into one "
           "shared local (colour + opacity decisions)";
    // ANTS-3457 — the std::lower_bound probe is precomputed per row, so the
    // per-cell isCellSearchMatch(globalLine, col) call must be gone entirely.
    EXPECT_EQ(body.count(QStringLiteral("isCellSearchMatch(globalLine, col)")), 0)
        << "paintEvent still probes isCellSearchMatch per cell — the "
           "std::lower_bound must be hoisted to a per-row precompute";
    EXPECT_TRUE(body.contains(QStringLiteral("m_paintSearchSpans")))
        << "paintEvent must precompute this row's search-match spans once "
           "into m_paintSearchSpans instead of a per-cell probe";
}

// INV-5 — paintEvent honors the QPaintEvent damage rect: it reads
// event->rect() and bounds the per-row shape+draw loop to the damaged band,
// so a partial update (the cursor blink invalidates one cell) no longer
// re-shapes all rows (ANTS-3454).
TEST(TerminalWidgetHotPathPerf, Inv5PaintHonorsDamageRect) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::paintEvent("));
    ASSERT_FALSE(body.isEmpty()) << "paintEvent not found";
    EXPECT_TRUE(body.contains(QStringLiteral("event->rect()")))
        << "paintEvent must read the QPaintEvent damage rect";
    EXPECT_TRUE(body.contains(QStringLiteral("vr = firstRow")))
        << "the per-row loop must start at the first damaged row";
    EXPECT_FALSE(body.contains(QStringLiteral("for (int vr = 0; vr < rows")))
        << "paintEvent still walks all rows unconditionally — the damage rect "
           "is ignored, so a cursor blink re-shapes the whole grid";
}

// INV-6 — updateSuggestion iterates the history through std::as_const
// (ANTS-4780). m_historyEntries is a member, and a range-for over a non-const
// Qt container calls the mutable begin(), which detaches when the container is
// shared. It is unshared today, so this pins the guarantee rather than a
// measured cost: without it, a later copy of the list silently turns this into
// a deep copy of the whole shell history on a path that runs per VT batch.
TEST(TerminalWidgetHotPathPerf, Inv6SuggestionScanDoesNotDetachHistory) {
    const QString body = functionBody(
        tw(), QStringLiteral("void TerminalWidget::updateSuggestion()"));
    ASSERT_FALSE(body.isEmpty()) << "updateSuggestion not found";
    EXPECT_TRUE(body.contains(QStringLiteral("std::as_const(m_historyEntries)")))
        << "updateSuggestion must iterate m_historyEntries through "
           "std::as_const — a bare range-for takes the detaching begin()";
    EXPECT_FALSE(body.contains(QStringLiteral(": m_historyEntries)")))
        << "updateSuggestion still range-fors the member directly";
}
