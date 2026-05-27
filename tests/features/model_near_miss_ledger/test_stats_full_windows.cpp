// ANTS-1894 INV-11 — statsFull windowing: window_24h.total filtered by ts ≤
// 24 h before nowMs AND project; all_time has no window filter;
// distinct_signatures counts post-sort distinct blocked_by arrays.
#include <gtest/gtest.h>
#include <QDateTime>
#include <QJsonArray>
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

TEST(ModelNearMissLedger, Inv11WindowExcludesOlderThan24h) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 hour = 60 * 60 * 1000;

    QList<NM::Record> recs;
    recs << makeRecord(now - 1 * hour,  {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now - 10 * hour, {QStringLiteral("composer_not_empty")});
    recs << makeRecord(now - 30 * hour, {QStringLiteral("composer_not_empty")});  // outside window
    recs << makeRecord(now - 48 * hour, {QStringLiteral("composer_not_empty")});  // outside window

    const QJsonObject env = NM::statsFull(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(env.value("window_24h").toObject().value("total").toInt(), 2);
    EXPECT_EQ(env.value("all_time").toObject().value("total").toInt(), 4);
}

TEST(ModelNearMissLedger, Inv11ProjectFilter) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")}, QStringLiteral("/p1"));
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")}, QStringLiteral("/p2"));

    const QJsonObject env = NM::statsFull(recs, QStringLiteral("/p1"), now);
    EXPECT_EQ(env.value("window_24h").toObject().value("total").toInt(), 1);
    EXPECT_EQ(env.value("all_time").toObject().value("total").toInt(), 1);

    // Global (empty projectScope) returns both.
    const QJsonObject envGlobal = NM::statsFull(recs, QString(), now);
    EXPECT_EQ(envGlobal.value("window_24h").toObject().value("total").toInt(), 2);
}

TEST(ModelNearMissLedger, Inv11DistinctSignatures) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    // Same post-sort signature [composer, dwell] (different evaluation order shouldn't matter)
    recs << makeRecord(now, {QStringLiteral("composer_not_empty"),
                             QStringLiteral("dwell_time_insufficient")});
    recs << makeRecord(now, {QStringLiteral("dwell_time_insufficient"),
                             QStringLiteral("composer_not_empty")});
    // Different signature [composer]
    recs << makeRecord(now, {QStringLiteral("composer_not_empty")});
    // Different signature [composer, override]
    recs << makeRecord(now, {QStringLiteral("composer_not_empty"),
                             QStringLiteral("override_cooldown_active")});

    const QJsonObject env = NM::statsFull(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(env.value("window_24h").toObject().value("distinct_signatures").toInt(), 3);
}
