// Feature-conformance test for ANTS-1735 — autonomous model-switch gate.
// See tests/features/model_auto_switch/spec.md and docs/specs/ANTS-1735.md.
//
// Covers the pure decision helper (INV-1..INV-9). No live terminal: every case
// builds a Gate value and asserts decide()/clampToFloor() output.

#include <gtest/gtest.h>
#include "modelautoswitch.h"

namespace {

using ModelRecommender::Tier;
using ModelAutoSwitch::Gate;
using ModelAutoSwitch::Decision;

// A Gate in which *every* condition is satisfied, so decide() acts. Each
// negative test below flips exactly one field, so the only thing under test in
// that case is the flipped gate.
//   current=Opus, recommended=Haiku, floor=Haiku → clamp=Haiku != Opus → act,
//   tierArg="haiku".
Gate actingGate() {
    Gate g;
    g.enabled          = true;
    g.focusedState     = ClaudeState::Idle;
    g.composerEmpty    = true;
    g.current          = Tier::Opus;
    g.recommended      = Tier::Haiku;
    g.floor            = Tier::Haiku;
    g.ticksTargetStable = ModelAutoSwitch::kStableTicks;
    g.msSinceLastSwitch = ModelAutoSwitch::kMinDwellMs;
    g.msSinceLastOverride = -1;   // ANTS-1890: no override on record
    return g;
}

std::string nm(Tier t) { return ModelRecommender::tierName(t).toStdString(); }

}  // namespace

// Baseline: the fully-satisfied gate acts, with the clamped-target alias.
TEST(ModelAutoSwitch, BaselineActsWithClampedAlias) {
    const Decision d = ModelAutoSwitch::decide(actingGate());
    EXPECT_TRUE(d.act);
    EXPECT_EQ(d.tierArg, QStringLiteral("haiku")) << d.tierArg.toStdString();
}

// INV-1: disabled → never acts, regardless of everything else.
TEST(ModelAutoSwitch, Inv1DisabledNeverActs) {
    Gate g = actingGate();
    g.enabled = false;
    EXPECT_FALSE(ModelAutoSwitch::decide(g).act);
}

// INV-2: any non-Idle focused state → never acts. One case per non-Idle
// ClaudeState value (claudeintegration.h:47-53).
TEST(ModelAutoSwitch, Inv2NonIdleNeverActs) {
    for (ClaudeState s : {ClaudeState::NotRunning, ClaudeState::Thinking,
                          ClaudeState::ToolUse, ClaudeState::Compacting}) {
        Gate g = actingGate();
        g.focusedState = s;
        EXPECT_FALSE(ModelAutoSwitch::decide(g).act)
            << "state index " << static_cast<int>(s);
    }
}

// INV-3: composer not provably empty → never acts.
TEST(ModelAutoSwitch, Inv3ComposerNonEmptyNeverActs) {
    Gate g = actingGate();
    g.composerEmpty = false;
    EXPECT_FALSE(ModelAutoSwitch::decide(g).act);
}

// ANTS-1908 — composer_not_empty soft-veto. When the composer carries
// text AND the user hasn't touched it for ≥ kComposerStaleVetoMs, the
// veto yields and the gate fires (long /loop sessions where leftover
// continuation-prompt text is sitting idle).
TEST(ModelAutoSwitch, Ants1908ComposerStaleVetoYields) {
    Gate g = actingGate();
    g.composerEmpty   = false;
    g.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs;
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(dec.act) << "stale composer should yield the veto";
    EXPECT_FALSE(dec.blockedBy.contains(QStringLiteral("composer_not_empty")))
        << "blockedBy should not carry composer_not_empty when stale";
}

// ANTS-1908 — recent keystroke keeps the hard veto.
TEST(ModelAutoSwitch, Ants1908FreshKeystrokeStillBlocks) {
    Gate g = actingGate();
    g.composerEmpty   = false;
    g.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs - 1;
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(dec.act);
    EXPECT_TRUE(dec.blockedBy.contains(QStringLiteral("composer_not_empty")));
}

// ANTS-1908 — sentinel -1 (no keystroke telemetry) preserves the
// pre-1908 hard-veto behaviour (caller hasn't wired the new field).
TEST(ModelAutoSwitch, Ants1908SentinelKeepsHardVeto) {
    Gate g = actingGate();
    g.composerEmpty   = false;
    g.composerStaleMs = -1;
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(dec.act);
    EXPECT_TRUE(dec.blockedBy.contains(QStringLiteral("composer_not_empty")));
}

// INV-4: clamped target already equals current → no-op (hysteresis on the
// clamped target).
TEST(ModelAutoSwitch, Inv4NoChangeNeverActs) {
    Gate g = actingGate();
    g.current = Tier::Haiku;       // clamp(Haiku,Haiku)=Haiku == current
    g.recommended = Tier::Haiku;
    g.floor = Tier::Haiku;
    EXPECT_FALSE(ModelAutoSwitch::decide(g).act);
}

// INV-5: stability threshold — below kStableTicks never acts; at it, acts.
TEST(ModelAutoSwitch, Inv5StabilityThreshold) {
    Gate below = actingGate();
    below.ticksTargetStable = ModelAutoSwitch::kStableTicks - 1;
    EXPECT_FALSE(ModelAutoSwitch::decide(below).act);

    Gate at = actingGate();
    at.ticksTargetStable = ModelAutoSwitch::kStableTicks;
    EXPECT_TRUE(ModelAutoSwitch::decide(at).act);
}

// INV-5: a persistent below-floor recommendation that clamps to current must
// NOT churn — stability is counted on the CLAMPED target, which equals current,
// so even a huge tick count never acts (no livelock).
TEST(ModelAutoSwitch, Inv5ClampedToCurrentNeverChurns) {
    Gate g = actingGate();
    g.floor = Tier::Sonnet;        // floor=sonnet
    g.current = Tier::Sonnet;      // already at floor
    g.recommended = Tier::Haiku;   // persistent Haiku rec → clamp=Sonnet==current
    g.ticksTargetStable = 1000;    // would churn if counted on the raw rec
    EXPECT_FALSE(ModelAutoSwitch::decide(g).act);
}

// INV-6: dwell threshold — below kMinDwellMs never acts; at it, acts.
TEST(ModelAutoSwitch, Inv6DwellThreshold) {
    Gate below = actingGate();
    below.msSinceLastSwitch = ModelAutoSwitch::kMinDwellMs - 1;
    EXPECT_FALSE(ModelAutoSwitch::decide(below).act);

    Gate at = actingGate();
    at.msSinceLastSwitch = ModelAutoSwitch::kMinDwellMs;
    EXPECT_TRUE(ModelAutoSwitch::decide(at).act);
}

// INV-7: when acting, tierArg is the lowercase alias of the CLAMPED target.
TEST(ModelAutoSwitch, Inv7TierArgIsClampedAlias) {
    {   // upgrade: current=Haiku, rec=Opus, floor=Haiku → opus
        Gate g = actingGate();
        g.current = Tier::Haiku; g.recommended = Tier::Opus; g.floor = Tier::Haiku;
        const Decision d = ModelAutoSwitch::decide(g);
        EXPECT_TRUE(d.act);
        EXPECT_EQ(d.tierArg, QStringLiteral("opus"));
    }
    {   // clamped downgrade: current=Opus, rec=Haiku, floor=Sonnet → sonnet
        Gate g = actingGate();
        g.current = Tier::Opus; g.recommended = Tier::Haiku; g.floor = Tier::Sonnet;
        const Decision d = ModelAutoSwitch::decide(g);
        EXPECT_TRUE(d.act);
        EXPECT_EQ(d.tierArg, QStringLiteral("sonnet"));
    }
    {   // full downgrade: current=Opus, rec=Haiku, floor=Haiku → haiku
        Gate g = actingGate();
        g.current = Tier::Opus; g.recommended = Tier::Haiku; g.floor = Tier::Haiku;
        const Decision d = ModelAutoSwitch::decide(g);
        EXPECT_TRUE(d.act);
        EXPECT_EQ(d.tierArg, QStringLiteral("haiku"));
    }
}

// INV-8: clampToFloor truth table — full rec×floor matrix. The result is the
// higher-ranked of (rec, floor); Opus is never clamped down.
TEST(ModelAutoSwitch, Inv8ClampTruthTable) {
    struct Case { Tier rec; Tier floor; Tier want; };
    const Case cases[] = {
        {Tier::Haiku,  Tier::Haiku,  Tier::Haiku},
        {Tier::Haiku,  Tier::Sonnet, Tier::Sonnet},
        {Tier::Haiku,  Tier::Opus,   Tier::Opus},
        {Tier::Sonnet, Tier::Haiku,  Tier::Sonnet},
        {Tier::Sonnet, Tier::Sonnet, Tier::Sonnet},
        {Tier::Sonnet, Tier::Opus,   Tier::Opus},
        {Tier::Opus,   Tier::Haiku,  Tier::Opus},
        {Tier::Opus,   Tier::Sonnet, Tier::Opus},
        {Tier::Opus,   Tier::Opus,   Tier::Opus},
    };
    for (const Case &c : cases) {
        EXPECT_EQ(ModelAutoSwitch::clampToFloor(c.rec, c.floor), c.want)
            << "clamp(" << nm(c.rec) << ", " << nm(c.floor) << ") = "
            << nm(ModelAutoSwitch::clampToFloor(c.rec, c.floor))
            << " want " << nm(c.want);
    }
}

// ----- ANTS-1890 — override cool-down + commit-intent clamp -----

// INV-6 — A project-scoped override on record within the 10-minute
// cool-down blocks decide() from acting, even when every other gate
// holds (dwell included).
TEST(ModelAutoSwitchCooldown, OverrideCoolDownBlocksAct) {
    Gate g = actingGate();
    g.msSinceLastOverride = ModelAutoSwitch::kOverrideCooldownMs - 1;
    const Decision d = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(d.act)
        << "An override on record within kOverrideCooldownMs must block act";
}

// INV-6 — Once the 10-minute cool-down has elapsed, decide() acts
// again (assuming every other gate still holds).
TEST(ModelAutoSwitchCooldown, OverrideCoolDownPassesWhenExpired) {
    Gate g = actingGate();
    g.msSinceLastOverride = ModelAutoSwitch::kOverrideCooldownMs;
    const Decision d = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(d.act)
        << "An expired-cool-down override must NOT block act";
}

// INV-6 — Sentinel -1 (no override on record / no ledger seen yet)
// does NOT block the cool-down rule, regardless of other field values.
TEST(ModelAutoSwitchCooldown, OverrideCoolDownIgnoredWhenSentinelMinusOne) {
    Gate g = actingGate();
    g.msSinceLastOverride = -1;
    const Decision d = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(d.act)
        << "Sentinel -1 must NOT block act (cool-down only applies when "
           "msSinceLastOverride >= 0)";
}

// INV-8 — Per-project scope. An override on a DIFFERENT project (which
// the cache key lookup returns as -1 sentinel) does not block this
// project's decide(). This is the gate-side property of INV-8: the
// per-project interpretation is how decide() reads msSinceLastOverride,
// not how the cache is populated.
TEST(ModelAutoSwitchCooldown, OverrideCoolDownIsPerProject) {
    Gate g = actingGate();
    // Caller looks up the focused tab's project in m_lastOverrideMsByProject.
    // For an unknown project key, the lookup returns -1 sentinel.
    g.msSinceLastOverride = -1;
    EXPECT_TRUE(ModelAutoSwitch::decide(g).act);
}

// INV-2a — Commit-intent recommendation (Tier::Haiku from the scorer)
// is clamped to the user's configured floor in the gate. Haiku with a
// Sonnet floor must yield tierArg="sonnet".
TEST(ModelAutoSwitchCooldown, CommitIntentTargetClampedByGate) {
    Gate g = actingGate();
    g.current     = Tier::Opus;
    g.recommended = Tier::Haiku;   // scorer's commit-intent override target
    g.floor       = Tier::Sonnet;  // user's configured floor
    const Decision d = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(d.act);
    EXPECT_EQ(d.tierArg, QStringLiteral("sonnet"))
        << "Haiku target must clamp up to Sonnet floor";
}

// INV-9: security boundary — tierArg of any acting decision is always one of the
// three fixed enum aliases, never arbitrary text.
TEST(ModelAutoSwitch, Inv9TierArgAlwaysEnumAlias) {
    for (Tier cur : {Tier::Haiku, Tier::Sonnet, Tier::Opus})
        for (Tier rec : {Tier::Haiku, Tier::Sonnet, Tier::Opus})
            for (Tier flr : {Tier::Haiku, Tier::Sonnet}) {
                Gate g = actingGate();
                g.current = cur; g.recommended = rec; g.floor = flr;
                const Decision d = ModelAutoSwitch::decide(g);
                if (d.act) {
                    EXPECT_TRUE(d.tierArg == QStringLiteral("haiku")
                             || d.tierArg == QStringLiteral("sonnet")
                             || d.tierArg == QStringLiteral("opus"))
                        << d.tierArg.toStdString();
                }
            }
}
