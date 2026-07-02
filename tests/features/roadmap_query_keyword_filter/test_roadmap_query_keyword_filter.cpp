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
