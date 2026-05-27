// ANTS-1894 INV-10 — statsSlim.dominantBlocker is the highest-occurrence
// token across the 24 h window (one count per distinct token in a record);
// ties broken by taxonomy order; empty window ⇒ empty string.
#include <gtest/gtest.h>
#include <QDateTime>
#include "modelnearmissledger.h"

namespace NM = ModelNearMissLedger;

namespace {

NM::Record makeRecord(const QStringList &blockers,
                      const QString &project = QStringLiteral("/p"),
                      qint64 atMs = 0) {
    NM::Record r;
    if (atMs == 0) atMs = QDateTime::currentMSecsSinceEpoch();
    r.ts              = QDateTime::fromMSecsSinceEpoch(atMs).toUTC().toString(Qt::ISODate);
    r.project         = project;
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("haiku");
    r.blockedBy       = blockers;
    return r;
}

}  // namespace

TEST(ModelNearMissLedger, Inv10ClearWinner) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    // 3 with composer_not_empty (sole) + 1 with override_cooldown (sole)
    recs << makeRecord({QStringLiteral("composer_not_empty")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("composer_not_empty")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("composer_not_empty")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("override_cooldown_active")}, QStringLiteral("/p"), now);

    const NM::StatsSlim s = NM::statsSlim(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(s.total24h, 4);
    EXPECT_EQ(s.dominantBlocker, QStringLiteral("composer_not_empty"));
}

TEST(ModelNearMissLedger, Inv10TieBrokenByTaxonomyOrder) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    // 2 each — focused_state (earlier in taxonomy) vs composer_not_empty
    recs << makeRecord({QStringLiteral("focused_state_not_idle")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("focused_state_not_idle")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("composer_not_empty")}, QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("composer_not_empty")}, QStringLiteral("/p"), now);

    const NM::StatsSlim s = NM::statsSlim(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(s.total24h, 4);
    EXPECT_EQ(s.dominantBlocker, QStringLiteral("focused_state_not_idle"));
}

TEST(ModelNearMissLedger, Inv10EmptyWindow) {
    const NM::StatsSlim s = NM::statsSlim({}, QStringLiteral("/p"),
                                          QDateTime::currentMSecsSinceEpoch());
    EXPECT_EQ(s.total24h, 0);
    EXPECT_TRUE(s.dominantBlocker.isEmpty());
}

// Records with multiple tokens contribute one count per distinct token.
TEST(ModelNearMissLedger, Inv10MultiTokenRecordContributesPerToken) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<NM::Record> recs;
    // 1 record with [composer, dwell, override]; 1 with [composer].
    // composer appears 2x; dwell + override 1x each.
    recs << makeRecord({QStringLiteral("composer_not_empty"),
                        QStringLiteral("dwell_time_insufficient"),
                        QStringLiteral("override_cooldown_active")},
                       QStringLiteral("/p"), now);
    recs << makeRecord({QStringLiteral("composer_not_empty")},
                       QStringLiteral("/p"), now);

    const NM::StatsSlim s = NM::statsSlim(recs, QStringLiteral("/p"), now);
    EXPECT_EQ(s.total24h, 2);
    EXPECT_EQ(s.dominantBlocker, QStringLiteral("composer_not_empty"));
}
