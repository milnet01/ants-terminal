// ANTS-1894 INV-4 — Decision.currentTier and recommendedTier are set on
// every decide() call regardless of act; recommendedTier is the *clamped*
// target (not the raw recommendation).
#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace MAS = ModelAutoSwitch;

// Block the gate (composerEmpty=false) but supply a non-default recommended
// that gets clamped UP by a Sonnet floor.
TEST(ModelNearMissLedger, Inv4TiersSetOnBlockedCall) {
    MAS::Gate g;
    g.enabled              = true;
    g.focusedState         = ClaudeState::Idle;
    g.composerEmpty        = false;                        // forces act=false
    g.current              = ModelRecommender::Tier::Opus;
    g.recommended          = ModelRecommender::Tier::Haiku;
    g.floor                = ModelRecommender::Tier::Sonnet;  // Haiku → clamped UP
    g.ticksTargetStable    = MAS::kStableTicks;
    g.msSinceLastSwitch    = MAS::kMinDwellMs * 2;
    g.configuredMinDwellMs = MAS::kMinDwellMs;
    g.msSinceLastOverride  = -1;

    const MAS::Decision d = MAS::decide(g);
    EXPECT_FALSE(d.act);
    EXPECT_EQ(d.currentTier, ModelRecommender::Tier::Opus);
    // recommendedTier is the CLAMPED target (Sonnet), not the raw Haiku rec.
    EXPECT_EQ(d.recommendedTier, ModelRecommender::Tier::Sonnet);
    EXPECT_NE(d.recommendedTier, ModelRecommender::Tier::Haiku);
}

// On the firing path, tiers are also set; tierArg matches recommendedTier.
TEST(ModelNearMissLedger, Inv4TiersSetOnFiringCall) {
    MAS::Gate g;
    g.enabled              = true;
    g.focusedState         = ClaudeState::Idle;
    g.composerEmpty        = true;
    g.current              = ModelRecommender::Tier::Opus;
    g.recommended          = ModelRecommender::Tier::Haiku;
    g.floor                = ModelRecommender::Tier::Haiku;
    g.ticksTargetStable    = MAS::kStableTicks;
    g.msSinceLastSwitch    = MAS::kMinDwellMs * 2;
    g.configuredMinDwellMs = MAS::kMinDwellMs;
    g.msSinceLastOverride  = -1;

    const MAS::Decision d = MAS::decide(g);
    ASSERT_TRUE(d.act);
    EXPECT_EQ(d.currentTier, ModelRecommender::Tier::Opus);
    EXPECT_EQ(d.recommendedTier, ModelRecommender::Tier::Haiku);
    EXPECT_EQ(d.tierArg, ModelRecommender::tierName(d.recommendedTier));
}
