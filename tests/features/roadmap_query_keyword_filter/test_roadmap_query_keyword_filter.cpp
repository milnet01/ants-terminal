// ANTS-3391 — feature-conformance test for the roadmap_query `query`
// keyword filter. Behavioural coverage of the pure mcp::bulletMatchesQuery
// matcher (Qt6::Core, in ants_core_lib) plus source-scrapes of
// remotecontrol.cpp (cmdRoadmapQuery wiring) and claudeintegration.cpp
// (schema property). See tests/features/roadmap_query_keyword_filter/spec.md.

#include "mcpprojection.h"   // mcp::bulletMatchesQuery

#include <gtest/gtest.h>

#include <QFile>
#include <QJsonObject>
#include <QString>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

namespace {

QString readSource(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// A bullet object shaped like the ones cmdRoadmapQuery filters
// (headline / headline_full / body are the matched surfaces).
QJsonObject bullet(const QString &headline, const QString &body,
                   const QString &headlineFull = {}) {
    QJsonObject o;
    o[QStringLiteral("headline")] = headline;
    o[QStringLiteral("body")]     = body;
    if (!headlineFull.isEmpty())
        o[QStringLiteral("headline_full")] = headlineFull;
    return o;
}

}  // namespace

// INV-1 — case-insensitive substring match on the headline, both directions.
TEST(RoadmapQueryKeywordFilter, Inv1HeadlineCaseInsensitive) {
    const QJsonObject b = bullet(QStringLiteral("Crash on resume"),
                                 QStringLiteral("Kind: fix."));
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("Crash")));
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("crash")));   // needle lower
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("RESUME"))); // needle upper
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("on res"))); // spans a space
}

// INV-2 — the body is matched, not just the headline.
TEST(RoadmapQueryKeywordFilter, Inv2MatchesBody) {
    const QJsonObject b = bullet(QStringLiteral("A short headline"),
                                 QStringLiteral("Lanes: terminalgrid. "
                                                "Fixes the wraparound glitch."));
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("wraparound")));
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("terminalgrid")));
    EXPECT_FALSE(mcp::bulletMatchesQuery(b, QStringLiteral("not-present")));
}

// INV-3 — headline_full (the untruncated surface) is matched.
TEST(RoadmapQueryKeywordFilter, Inv3MatchesHeadlineFull) {
    const QJsonObject b = bullet(
        QStringLiteral("Truncated headline that stops at the cap"),
        QStringLiteral("Kind: refactor."),
        QStringLiteral("Truncated headline that stops at the cap "
                       "then continues with the pagination rewrite"));
    // "pagination" lives only in headline_full, past the visible headline.
    EXPECT_TRUE(mcp::bulletMatchesQuery(b, QStringLiteral("pagination")));
}

// INV-4 — a needle absent from all three surfaces returns false.
TEST(RoadmapQueryKeywordFilter, Inv4NoMatch) {
    const QJsonObject b = bullet(QStringLiteral("Alpha"),
                                 QStringLiteral("Beta gamma."));
    EXPECT_FALSE(mcp::bulletMatchesQuery(b, QStringLiteral("delta")));
    // Missing fields stringify to "" and don't spuriously match.
    QJsonObject bare;
    bare[QStringLiteral("id")] = QStringLiteral("ANTS-1");
    EXPECT_FALSE(mcp::bulletMatchesQuery(bare, QStringLiteral("x")));
}

// INV-5 — cmdRoadmapQuery wires the filter on both branches, guards the
// incompatible combos, and echoes the applied query.
TEST(RoadmapQueryKeywordFilter, Inv5HandlerWiring) {
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("applyQueryFilter")))
        << "query filter helper must be applied in cmdRoadmapQuery";
    EXPECT_TRUE(rc.contains(QStringLiteral("mcp::bulletMatchesQuery(")))
        << "handler must delegate to the shared pure matcher";
    EXPECT_TRUE(rc.contains(QStringLiteral(
        "query keyword filter does not combine")))
        << "bad_mode_combo guard for query + targeted/aggregate surfaces";
    EXPECT_TRUE(rc.contains(QStringLiteral("out[\"query\"] = queryArg")))
        << "the applied query must be echoed in the envelope";
}

// INV-8 (ANTS-3560) — when a keyword `query` matches zero bullets on a
// roadmap that DOES contain id-bearing bullets, the handler must emit a
// query-specific "no bullet matched query" warning, not the ANTS-1538
// "every entry has no [PROJ-NNNN] id" text (which misdirects toward
// include_narrator_bullets). The gate splits on the post-ID-prune count
// captured before the query filter runs.
TEST(RoadmapQueryKeywordFilter, Inv8QueryEmptyWarningIsQueryAware) {
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("postIdPruneCountFull")))
        << "warning gate must measure the id-bearing count before the "
           "query filter to distinguish query-miss from id-mandate drop";
    EXPECT_TRUE(rc.contains(QStringLiteral("no bullet matched query")))
        << "a zero-match query on an id-bearing roadmap must warn about "
           "the query, not claim every entry lacks a [PROJ-NNNN] id";
}

// INV-9 (ANTS-4423) — the SECTION path carries INV-8's query-aware warning
// too. ANTS-3560 fixed the text on the full-file branch and left `section=`
// with the bare ANTS-1538 wording, so half its own surface kept the defect:
// a section query that matched nothing still claimed every bullet lacked a
// [PROJ-NNNN] id and prescribed include_narrator_bullets /
// include_section_headers, neither of which can help.
//
// Reproduced in-session on this project's own ROADMAP.md (2026-08-17):
// 40 bullets in the target section, every one id-bearing, count 0, and the
// warning blamed the ID filter. A control query on a keyword that DOES occur
// returned 37 of the 40, which is what proves the keyword filter — not the
// ID filter — emptied the set.
//
// Scraped rather than driven for the same reason INV-8 is: the warning is
// composed inside cmdRoadmapQuery's section branch, which no unit seam
// reaches. Both anchors are needed — the count alone could exist unused, and
// the text alone could be the full-file literal INV-8 already pins.
TEST(RoadmapQueryKeywordFilter, Inv9SectionPathWarningIsQueryAware) {
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    EXPECT_TRUE(rc.contains(QStringLiteral("postIdPruneCountSec")))
        << "the section= branch must measure its own id-bearing count before "
           "the query filter; postIdPruneCountFull is the full-file path's "
           "and says nothing about a section query";
    EXPECT_TRUE(rc.contains(QStringLiteral(
        "no bullet in this section matched query")))
        << "a zero-match query on an id-bearing SECTION must warn about the "
           "query, not claim every entry lacks a [PROJ-NNNN] id";
}

// INV-6 — the inputSchema declares the `query` property so the dispatch
// layer recognises it (no longer flagged in ignored_args).
TEST(RoadmapQueryKeywordFilter, Inv6SchemaDeclaresQuery) {
    const QString ci = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"query\"]")))
        << "roadmap_query inputSchema must declare the query property";
}

// INV-7 (ANTS-3420) — `query` (and the ANTS-3402 / ANTS-1907 companions)
// must reach cmdRoadmapQuery. ANTS-3422 replaced the hand-maintained
// per-arg forward — whose repeated omissions dropped `query` and its
// companions at the MCP boundary — with a verbatim `rcDelegate` forward
// that passes the whole args object through, so every arg (present and
// future) arrives by construction. Guards against a regression back to a
// selective forward that could re-drop an arg.
TEST(RoadmapQueryKeywordFilter, Inv7DispatchForwardsQuery) {
    const QString mw = readSource(SRC_MAINWINDOW_CPP);
    ASSERT_FALSE(mw.isEmpty());
    EXPECT_TRUE(mw.contains(
        QStringLiteral("rcDelegate(&RemoteControl::cmdRoadmapQuery)")))
        << "dispatch: roadmap_query registered via the verbatim rcDelegate "
           "forward, so query + max_body_bytes + include_section_etags + "
           "section_etag_match all reach the handler";
    EXPECT_TRUE(mw.contains(QStringLiteral("ANTS-3422")))
        << "dispatch: ANTS-3422 verbatim-forward anchor present";
}
