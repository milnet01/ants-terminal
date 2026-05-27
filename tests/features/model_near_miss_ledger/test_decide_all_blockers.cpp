// ANTS-1894 INV-2 — each of the 7 guards appends its canonical token on
// failure; tokens land in evaluation order. Table-driven over a
// representative subset of the 2^7 guard combinations.
#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace MAS = ModelAutoSwitch;

namespace {

MAS::Gate okGate() {
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

// Each guard, in isolation, appends exactly its one canonical token.
TEST(ModelNearMissLedger, Inv2SingleGuardTokens) {
    {
        MAS::Gate g = okGate(); g.enabled = false;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("auto_switch_disabled"));
    }
    {
        MAS::Gate g = okGate(); g.focusedState = ClaudeState::Thinking;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("focused_state_not_idle"));
    }
    {
        MAS::Gate g = okGate(); g.composerEmpty = false;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("composer_not_empty"));
    }
    {
        // target == current (recommended == current after clamp)
        MAS::Gate g = okGate(); g.recommended = ModelRecommender::Tier::Opus;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("target_equals_current"));
    }
    {
        MAS::Gate g = okGate(); g.ticksTargetStable = 0;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("ticks_target_stable_insufficient"));
    }
    {
        MAS::Gate g = okGate(); g.msSinceLastSwitch = 0;
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("dwell_time_insufficient"));
    }
    {
        MAS::Gate g = okGate(); g.msSinceLastOverride = 1000;  // within 10 min
        const auto bb = MAS::decide(g).blockedBy;
        ASSERT_EQ(bb.size(), 1);
        EXPECT_EQ(bb[0], QStringLiteral("override_cooldown_active"));
    }
}

// Multiple guards failing → tokens in evaluation order.
TEST(ModelNearMissLedger, Inv2MultipleBlockersInOrder) {
    MAS::Gate g = okGate();
    g.composerEmpty       = false;   // 3rd
    g.ticksTargetStable   = 0;       // 5th
    g.msSinceLastOverride = 1000;    // 7th
    const auto bb = MAS::decide(g).blockedBy;
    ASSERT_EQ(bb.size(), 3);
    EXPECT_EQ(bb[0], QStringLiteral("composer_not_empty"));
    EXPECT_EQ(bb[1], QStringLiteral("ticks_target_stable_insufficient"));
    EXPECT_EQ(bb[2], QStringLiteral("override_cooldown_active"));
}

// All 7 guards failing → all 7 tokens, in order.
TEST(ModelNearMissLedger, Inv2AllSevenBlockers) {
    MAS::Gate g;  // default-constructed: enabled=false, focusedState=NotRunning,
                  // composerEmpty=true, ticksTargetStable=0, msSinceLastSwitch=0
    g.composerEmpty       = false;
    g.recommended         = ModelRecommender::Tier::Haiku;
    g.current             = ModelRecommender::Tier::Haiku;   // target == current
    g.msSinceLastOverride = 1000;
    g.configuredMinDwellMs = MAS::kMinDwellMs;
    const auto bb = MAS::decide(g).blockedBy;
    ASSERT_EQ(bb.size(), 7);
    EXPECT_EQ(bb[0], QStringLiteral("auto_switch_disabled"));
    EXPECT_EQ(bb[1], QStringLiteral("focused_state_not_idle"));
    EXPECT_EQ(bb[2], QStringLiteral("composer_not_empty"));
    EXPECT_EQ(bb[3], QStringLiteral("target_equals_current"));
    EXPECT_EQ(bb[4], QStringLiteral("ticks_target_stable_insufficient"));
    EXPECT_EQ(bb[5], QStringLiteral("dwell_time_insufficient"));
    EXPECT_EQ(bb[6], QStringLiteral("override_cooldown_active"));
}
