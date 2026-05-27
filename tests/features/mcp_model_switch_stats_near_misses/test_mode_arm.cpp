// ANTS-1894 INV-12 — model_switch_stats envelope: default mode carries a
// slim near_misses block alongside firing fields + config triple + scope
// echo; mode:"near_misses" replaces firing fields with the full breakdown
// but preserves the config triple + scope echo; mode:"bogus" refuses.
//
// This is a pure unit test against the aggregation surface
// (ModelNearMissLedger::statsSlim / statsFull). The dispatcher-level test
// (an MCP call through RemoteControl::cmdModelSwitchStats) requires
// MainWindow plumbing the unit test bundle doesn't have, so the contract
// here covers the data-shape predicates the dispatcher serializes.
#include <gtest/gtest.h>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include "modelnearmissledger.h"

namespace NM = ModelNearMissLedger;

namespace {

NM::Record makeRecord(qint64 tsMs,
                      const QStringList &blockers,
                      const QString &project = QStringLiteral("/p")) {
    NM::Record r;
    r.ts              = QDateTime::fromMSecsSinceEpoch(tsMs).toUTC().toString(Qt::ISODate);
    r.project         = project;
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("haiku");
    r.blockedBy       = blockers;
    return r;
}

}  // namespace

// Default (slim) block: total_24h + dominant_blocker.
TEST(McpModelSwitchStatsNearMisses, SlimBlockShape) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now, {QStringLiteral("dwell_time_insufficient")});

    const NM::StatsSlim s = NM::statsSlim(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(s.total24h, 3);
    EXPECT_EQ(s.dominantBlocker, QStringLiteral("composer_not_empty"));
}

// Full envelope (mode:"near_misses") carries window_24h + all_time + headline.
TEST(McpModelSwitchStatsNearMisses, FullEnvelopeShape) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 hour = 60 * 60 * 1000;
    QList<NM::Record> recs;
    recs << makeRecord(now - 1 * hour, {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now - 2 * hour, {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now - 30 * hour, {QStringLiteral("override_cooldown_active")});  // outside 24h

    const QJsonObject env = NM::statsFull(recs, QStringLiteral("/p"), now);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("mode").toString(), QStringLiteral("near_misses"));
    EXPECT_EQ(env.value("scope").toString(), QStringLiteral("project"));

    const QJsonObject w24 = env.value("window_24h").toObject();
    EXPECT_EQ(w24.value("total").toInt(), 2);
    EXPECT_EQ(w24.value("dominant_blocker").toString(), QStringLiteral("composer_not_empty"));
    EXPECT_EQ(w24.value("by_blocked_by").toObject().value("composer_not_empty").toInt(), 2);

    const QJsonObject allT = env.value("all_time").toObject();
    EXPECT_EQ(allT.value("total").toInt(), 3);

    EXPECT_TRUE(env.contains("headline"));
    EXPECT_TRUE(env.value("headline").toString().contains(QStringLiteral("composer_not_empty")));
}

// Global scope: empty projectScope aggregates across projects.
TEST(McpModelSwitchStatsNearMisses, GlobalScopeAggregatesAcrossProjects) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")}, QStringLiteral("/p1"));
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")}, QStringLiteral("/p2"));
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")}, QStringLiteral("/p3"));

    const QJsonObject env = NM::statsFull(recs, QString(), now);
    EXPECT_EQ(env.value("scope").toString(), QStringLiteral("global"));
    EXPECT_EQ(env.value("window_24h").toObject().value("total").toInt(), 3);
}

// Empty window gives a sensible "0 near-misses" headline.
TEST(McpModelSwitchStatsNearMisses, EmptyWindowHeadline) {
    const QJsonObject env = NM::statsFull(
        {}, QStringLiteral("/p"), QDateTime::currentMSecsSinceEpoch());
    EXPECT_EQ(env.value("window_24h").toObject().value("total").toInt(), 0);
    EXPECT_EQ(env.value("headline").toString(),
              QStringLiteral("0 near-misses in last 24 h"));
}
