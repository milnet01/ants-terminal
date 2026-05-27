// ANTS-1894 INV-3 — effective dwell floor is max(kMinDwellMs,
// configuredMinDwellMs). Exercised through decide()'s dwell_time_insufficient
// blocker (the helper itself is file-static).
#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace MAS = ModelAutoSwitch;

namespace {

MAS::Gate baseGate() {
    MAS::Gate g;
    g.enabled              = true;
    g.focusedState         = ClaudeState::Idle;
    g.composerEmpty        = true;
    g.current              = ModelRecommender::Tier::Opus;
    g.recommended          = ModelRecommender::Tier::Haiku;
    g.floor                = ModelRecommender::Tier::Haiku;
    g.ticksTargetStable    = MAS::kStableTicks;
    g.msSinceLastOverride  = -1;
    return g;
}

bool dwellBlocked(const MAS::Gate &g) {
    return MAS::decide(g).blockedBy.contains(QStringLiteral("dwell_time_insufficient"));
}

}  // namespace

// configuredMinDwellMs = 0 → effective floor remains kMinDwellMs (90 s).
TEST(ModelNearMissLedger, Inv3DegenerateConfigStillRespectsHardFloor) {
    MAS::Gate g = baseGate();
    g.configuredMinDwellMs = 0;

    g.msSinceLastSwitch = 60'000;   // 60 s < 90 s
    EXPECT_TRUE(dwellBlocked(g));

    g.msSinceLastSwitch = 95'000;   // 95 s > 90 s
    EXPECT_FALSE(dwellBlocked(g));
}

// configuredMinDwellMs = 300_000 (5 min) → 5 min required even when above 90 s.
TEST(ModelNearMissLedger, Inv3ConfigHigherThanHardFloor) {
    MAS::Gate g = baseGate();
    g.configuredMinDwellMs = 300'000;

    g.msSinceLastSwitch = 100'000;   // 100 s < 300 s
    EXPECT_TRUE(dwellBlocked(g));

    g.msSinceLastSwitch = 310'000;   // 310 s > 300 s
    EXPECT_FALSE(dwellBlocked(g));
}

// Default (configuredMinDwellMs == kMinDwellMs) → pre-1894 behaviour.
TEST(ModelNearMissLedger, Inv3DefaultConfigMatchesHardFloor) {
    MAS::Gate g = baseGate();
    // Don't set configuredMinDwellMs — the default kMinDwellMs applies.
    g.msSinceLastSwitch = MAS::kMinDwellMs - 1;
    EXPECT_TRUE(dwellBlocked(g));
    g.msSinceLastSwitch = MAS::kMinDwellMs + 1;
    EXPECT_FALSE(dwellBlocked(g));
}
