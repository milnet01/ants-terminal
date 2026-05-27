// ANTS-1894 INV-13 — fromJson(legacy partial JSON) uses struct defaults;
// idempotent re-serialisation holds for writer-produced records.
#include <gtest/gtest.h>
#include <QJsonDocument>
#include <QJsonObject>
#include "modelnearmissledger.h"

namespace NM = ModelNearMissLedger;

TEST(ModelNearMissLedger, Inv13MissingFieldsDefault) {
    // Minimal legacy JSON — only the required fields present.
    QJsonObject o;
    o[QStringLiteral("ts")]               = QStringLiteral("2026-05-27T09:30:00Z");
    o[QStringLiteral("project")]          = QStringLiteral("/p");
    o[QStringLiteral("current_tier")]     = QStringLiteral("opus");
    o[QStringLiteral("recommended_tier")] = QStringLiteral("haiku");

    const NM::Record r = NM::fromJson(o);
    EXPECT_EQ(r.ts, QStringLiteral("2026-05-27T09:30:00Z"));
    EXPECT_EQ(r.project, QStringLiteral("/p"));
    // Defaults from the struct:
    EXPECT_TRUE(r.composerEmpty);          // default true
    EXPECT_EQ(r.ticksTargetStable, 0);
    EXPECT_EQ(r.dwellMs, 0);
    EXPECT_EQ(r.msSinceLastOverride, -1);   // sentinel preserved on missing key
    EXPECT_TRUE(r.blockedBy.isEmpty());
    EXPECT_TRUE(r.focusedState.isEmpty());
    EXPECT_TRUE(r.sessionId.isEmpty());
}

TEST(ModelNearMissLedger, Inv13IdempotentRoundTrip) {
    // Writer-produced record round-trips losslessly through toJson/fromJson.
    NM::Record r;
    r.ts              = QStringLiteral("2026-05-27T09:30:00Z");
    r.sessionId       = QStringLiteral("abc");
    r.project         = QStringLiteral("/proj");
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("sonnet");
    r.blockedBy       = {QStringLiteral("composer_not_empty"),
                         QStringLiteral("dwell_time_insufficient")};
    r.composerEmpty   = false;
    r.focusedState    = QStringLiteral("thinking");
    r.ticksTargetStable = 3;
    r.dwellMs           = 75000;
    r.msSinceLastOverride = 120000;

    const QJsonObject once  = NM::toJson(r);
    const NM::Record  back  = NM::fromJson(once);
    const QJsonObject twice = NM::toJson(back);
    EXPECT_EQ(once, twice);

    EXPECT_EQ(back.sessionId, r.sessionId);
    EXPECT_EQ(back.project, r.project);
    EXPECT_EQ(back.blockedBy, r.blockedBy);
    EXPECT_EQ(back.composerEmpty, r.composerEmpty);
    EXPECT_EQ(back.focusedState, r.focusedState);
    EXPECT_EQ(back.ticksTargetStable, r.ticksTargetStable);
    EXPECT_EQ(back.dwellMs, r.dwellMs);
    EXPECT_EQ(back.msSinceLastOverride, r.msSinceLastOverride);
}

TEST(ModelNearMissLedger, Inv13PreservesMsSinceLastOverrideZero) {
    // 0 is a valid value (override happened "just now"); must NOT be
    // collapsed to the -1 sentinel by the round-trip.
    QJsonObject o;
    o[QStringLiteral("ts")]                    = QStringLiteral("2026-05-27T09:30:00Z");
    o[QStringLiteral("project")]               = QStringLiteral("/p");
    o[QStringLiteral("ms_since_last_override")]= 0.0;

    const NM::Record r = NM::fromJson(o);
    EXPECT_EQ(r.msSinceLastOverride, 0);
}
