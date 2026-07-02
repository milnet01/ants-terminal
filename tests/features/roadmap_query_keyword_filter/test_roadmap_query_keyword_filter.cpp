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
    const QString rc = readSource(SRC_RC_CPP);
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

// INV-6 — the inputSchema declares the `query` property so the dispatch
// layer recognises it (no longer flagged in ignored_args).
TEST(RoadmapQueryKeywordFilter, Inv6SchemaDeclaresQuery) {
    const QString ci = readSource(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    EXPECT_TRUE(ci.contains(QStringLiteral("props[\"query\"]")))
        << "roadmap_query inputSchema must declare the query property";
}

// INV-7 (ANTS-3420) — the mainwindow dispatch lambda must FORWARD `query`
// into the cmdRoadmapQuery req. This is the gap the ANTS-3391 coverage
// missed: the handler wiring (INV-5) + schema (INV-6) were present, but
// the arg was dropped at the MCP boundary before the handler saw it, so
// the filter was inert end-to-end. Mirrors the sibling DispatchForwards*
// scrapes (ANTS-1437 mode / ANTS-1586 include_body). Guards against a
// regression that re-drops the arg from the hand-maintained forward list.
TEST(RoadmapQueryKeywordFilter, Inv7DispatchForwardsQuery) {
    const QString mw = readSource(SRC_MAINWINDOW_CPP);
    ASSERT_FALSE(mw.isEmpty());
    EXPECT_TRUE(mw.contains(QStringLiteral("ANTS-3420 — forward `query`")))
        << "dispatch: ANTS-3420 query-forward anchor present";
    EXPECT_TRUE(mw.contains(QStringLiteral("args.value(\"query\")")))
        << "dispatch: lambda reads query from args";
    EXPECT_TRUE(mw.contains(QStringLiteral("req[\"query\"]")))
        << "dispatch: lambda writes query into the cmdRoadmapQuery req";
    // Companion drops fixed alongside (ANTS-3402 / ANTS-1907): each must
    // reach the handler too, or its feature stays inert over MCP.
    EXPECT_TRUE(mw.contains(QStringLiteral("req[\"max_body_bytes\"]")))
        << "dispatch: lambda forwards max_body_bytes (ANTS-3402)";
    EXPECT_TRUE(mw.contains(QStringLiteral("req[\"include_section_etags\"]")))
        << "dispatch: lambda forwards include_section_etags (ANTS-1907)";
    EXPECT_TRUE(mw.contains(QStringLiteral("req[\"section_etag_match\"]")))
        << "dispatch: lambda forwards section_etag_match (ANTS-1907)";
}
