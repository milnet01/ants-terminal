// Feature-conformance test for ANTS-1974 — user-selectable home/baseline tier.
// See tests/features/model_home_tier/spec.md and docs/specs/ANTS-1974.md.
//
// Pure: tierForScore() is a free function; score() is exercised only against a
// non-existent path (absent transcript ⇒ home tier).

#include <gtest/gtest.h>
#include <QString>
#include "modelrecommender.h"

using ModelRecommender::Tier;
using ModelRecommender::tierForScore;

// INV-1 — home=Sonnet reproduces the pre-1974 mapping exactly.
TEST(ModelHomeTier, Inv1SonnetHomeReproducesLegacy) {
    // up
    EXPECT_EQ(tierForScore(2,  false, Tier::Sonnet), Tier::Opus);
    EXPECT_EQ(tierForScore(5,  true,  Tier::Sonnet), Tier::Opus);
    // neutral
    EXPECT_EQ(tierForScore(1,  false, Tier::Sonnet), Tier::Sonnet);
    EXPECT_EQ(tierForScore(0,  false, Tier::Sonnet), Tier::Sonnet);
    EXPECT_EQ(tierForScore(-1, false, Tier::Sonnet), Tier::Sonnet);
    // down — both mechanical values collapse to Haiku at a Sonnet home
    EXPECT_EQ(tierForScore(-2, false, Tier::Sonnet), Tier::Haiku);
    EXPECT_EQ(tierForScore(-2, true,  Tier::Sonnet), Tier::Haiku);
    EXPECT_EQ(tierForScore(-9, true,  Tier::Sonnet), Tier::Haiku);
}

// INV-2 — neutral band returns exactly home, every home.
TEST(ModelHomeTier, Inv2NeutralReturnsHome) {
    for (Tier home : {Tier::Haiku, Tier::Sonnet, Tier::Opus}) {
        EXPECT_EQ(tierForScore(0,  false, home), home);
        EXPECT_EQ(tierForScore(1,  true,  home), home);
        EXPECT_EQ(tierForScore(-1, false, home), home);
    }
}

// INV-3 — up arm never exceeds Opus.
TEST(ModelHomeTier, Inv3UpClampsAtOpus) {
    EXPECT_EQ(tierForScore(2, false, Tier::Opus), Tier::Opus);
    EXPECT_EQ(tierForScore(9, true,  Tier::Opus), Tier::Opus);
    EXPECT_EQ(tierForScore(2, false, Tier::Sonnet), Tier::Opus);  // sonnet+1
}

// INV-4 — down arms never go below Haiku.
TEST(ModelHomeTier, Inv4DownClampsAtHaiku) {
    EXPECT_EQ(tierForScore(-2, false, Tier::Haiku), Tier::Haiku);
    EXPECT_EQ(tierForScore(-2, true,  Tier::Haiku), Tier::Haiku);
    EXPECT_EQ(tierForScore(-9, true,  Tier::Haiku), Tier::Haiku);
}

// INV-5 — home=Opus: mild-down→Sonnet, strong-down(mechanical)→Haiku.
TEST(ModelHomeTier, Inv5OpusHomeDowngradePath) {
    EXPECT_EQ(tierForScore(-2, false, Tier::Opus), Tier::Sonnet);  // mild
    EXPECT_EQ(tierForScore(-3, false, Tier::Opus), Tier::Sonnet);
    EXPECT_EQ(tierForScore(-2, true,  Tier::Opus), Tier::Haiku);   // strong
    // up + neutral from Opus home
    EXPECT_EQ(tierForScore(2,  false, Tier::Opus), Tier::Opus);
    EXPECT_EQ(tierForScore(0,  false, Tier::Opus), Tier::Opus);
}

// INV-8 — score() on an absent transcript returns the home tier; default arg
// is Sonnet (back-compat). Distinct home tiers give distinct results for the
// same (non-existent) path — the home flows through, not a hardcoded default.
TEST(ModelHomeTier, Inv8AbsentTranscriptReturnsHome) {
    const QString missing = QStringLiteral("/nonexistent/ants-1974-no-such-transcript.jsonl");
    EXPECT_EQ(ModelRecommender::score(missing).tier, Tier::Sonnet);          // default arg
    EXPECT_EQ(ModelRecommender::score(missing, Tier::Opus).tier, Tier::Opus);
    EXPECT_EQ(ModelRecommender::score(missing, Tier::Haiku).tier, Tier::Haiku);
}
