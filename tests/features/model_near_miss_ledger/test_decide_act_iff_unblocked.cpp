// ANTS-1894 INV-1 — decide() returns act=true iff blockedBy.isEmpty();
// when act, tierArg == tierName(recommendedTier). Preserves the pre-1894
// firing-side contract.
#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace MAS = ModelAutoSwitch;

namespace {

MAS::Gate makeUnblockedGate() {
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
    return g;
}

}  // namespace

TEST(ModelNearMissLedger, Inv1ActIffUnblocked) {
    // Unblocked gate → act=true, tierArg == tierName(clamped target).
    {
        const MAS::Decision d = MAS::decide(makeUnblockedGate());
        EXPECT_TRUE(d.act);
        EXPECT_TRUE(d.blockedBy.isEmpty());
        EXPECT_EQ(d.tierArg, ModelRecommender::tierName(ModelRecommender::Tier::Haiku));
        EXPECT_EQ(d.recommendedTier, ModelRecommender::Tier::Haiku);
    }
    // Block composer → act=false, blockedBy non-empty, tierArg empty.
    {
        MAS::Gate g = makeUnblockedGate();
        g.composerEmpty = false;
        const MAS::Decision d = MAS::decide(g);
        EXPECT_FALSE(d.act);
        EXPECT_FALSE(d.blockedBy.isEmpty());
        EXPECT_TRUE(d.tierArg.isEmpty());
    }
}
