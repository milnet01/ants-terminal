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

// ANTS-1939 — focused_state_not_idle soft-veto. When the agent is busy
// (state != Idle) but the human hasn't typed for ≥ the stale threshold, the
// veto yields: an autonomous-loop window where a cheaper model saves the most.
TEST(ModelAutoSwitch, Ants1939FocusedStateHumanIdleYields) {
    Gate g = actingGate();
    g.focusedState  = ClaudeState::ToolUse;       // agent grinding away
    g.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs;  // human idle
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(dec.act) << "human-idle agent-active window should yield";
    EXPECT_FALSE(dec.blockedBy.contains(QStringLiteral("focused_state_not_idle")))
        << "blockedBy should not carry focused_state_not_idle when human idle";
}

// ANTS-1939 — a recent keystroke while the agent is busy keeps the hard veto
// (the human is actively driving; don't switch under them).
TEST(ModelAutoSwitch, Ants1939FreshKeystrokeStillBlocksFocusedState) {
    Gate g = actingGate();
    g.focusedState  = ClaudeState::ToolUse;
    g.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs - 1;
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(dec.act);
    EXPECT_TRUE(dec.blockedBy.contains(QStringLiteral("focused_state_not_idle")));
}

// ANTS-1939 — sentinel -1 (no keystroke telemetry) preserves the legacy
// hard-veto behaviour for the focused_state gate.
TEST(ModelAutoSwitch, Ants1939SentinelKeepsHardVetoFocusedState) {
    Gate g = actingGate();
    g.focusedState  = ClaudeState::Thinking;
    g.composerStaleMs = -1;
    const auto dec = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(dec.act);
    EXPECT_TRUE(dec.blockedBy.contains(QStringLiteral("focused_state_not_idle")));
}

// ANTS-1939 — composes with ANTS-1917 idle_end_of_session. The two gates
// cover complementary cases: focused_state fires when NOT idle, idle gate
// when idle. A human-idle agent-active window (state != Idle, idleElapsedMs
// == -1 because the shell isn't idle) acts WITHOUT re-enabling end-of-session
// tail switches. Conversely a genuine end-of-session idle (state == Idle,
// idleElapsedMs past ceiling) still blocks.
TEST(ModelAutoSwitch, Ants1939ComposesWithIdleEndOfSession) {
    // Agent-active, human-idle: focused_state yields, idle gate dormant → act.
    Gate active = actingGate();
    active.focusedState  = ClaudeState::ToolUse;
    active.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs;
    active.idleElapsedMs = -1;                   // shell not idle
    EXPECT_TRUE(ModelAutoSwitch::decide(active).act);

    // End-of-session: shell idle past the ceiling → idle_end_of_session blocks
    // even though focused_state is Idle and the human is long gone.
    Gate eos = actingGate();
    eos.focusedState  = ClaudeState::Idle;
    eos.composerStaleMs = ModelAutoSwitch::kComposerStaleVetoMs;
    eos.idleElapsedMs = ModelAutoSwitch::kIdleEndOfSessionMs;
    const auto dec = ModelAutoSwitch::decide(eos);
    EXPECT_FALSE(dec.act);
    EXPECT_TRUE(dec.blockedBy.contains(QStringLiteral("idle_end_of_session")));
}

// ANTS-1940 — regret-driven conservatism multiplier truth table.
TEST(ModelAutoSwitch, Ants1940ConservatismMultiplier) {
    using ModelAutoSwitch::conservatismDwellMultiplier;
    const int floor = 10;
    // Past the floor → no penalty regardless of regret.
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(10, 90, floor, false), 1.0);
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(15, 90, floor, true), 1.0);
    // Calibrating with low regret → no penalty.
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(4, 40, floor, false), 1.0);
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(4, 30, floor, false), 1.0);
    // Calibrating with high regret, non-mechanical → full multiplier.
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(4, 50, floor, false),
                     ModelAutoSwitch::kCalibrationDwellMult);
    // Calibrating with high regret, mechanical → half the penalty (safe window).
    EXPECT_DOUBLE_EQ(conservatismDwellMultiplier(4, 50, floor, true),
                     1.0 + (ModelAutoSwitch::kCalibrationDwellMult - 1.0) * 0.5);
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

// ---- ANTS-1928: advanceStability tier-lock window ------------------------
// The pure accrual helper folds one tick's clamped recommendation in. These
// table tests reproduce the production oscillation patterns that left the
// switcher stuck at `ticks_target_stable_insufficient` (the dominant
// near-miss blocker) and assert the tier-lock window lets a candidate reach
// kStableTicks despite brief reversions.

using ModelAutoSwitch::StabilityState;
using ModelAutoSwitch::advanceStability;

namespace {
// Tick spacing — the production status timer fires every 2 s.
constexpr qint64 kTick = 2'000;
}  // namespace

// Two consecutive supporting ticks accrue to kStableTicks (baseline, no noise).
TEST(ModelAutoSwitchStability, ConsecutiveSupportAccrues) {
    StabilityState s;
    s = advanceStability(s, Tier::Opus, Tier::Sonnet, 0);
    EXPECT_EQ(s.ticksStable, 1);
    s = advanceStability(s, Tier::Opus, Tier::Sonnet, kTick);
    EXPECT_EQ(s.ticksStable, 2);
    EXPECT_GE(s.ticksStable, ModelAutoSwitch::kStableTicks);
}

// THE PRODUCTION BUG: a 1-in-3 duty cycle (candidate, revert, revert, candidate)
// must still reach kStableTicks — the tier-lock window holds accrual across the
// two reversions because they fall inside kTierLockWindowMs of the candidate's
// first appearance. Pre-1928 this reset at the 2nd reversion and never fired.
TEST(ModelAutoSwitchStability, OscillationWithinWindowStillAccrues) {
    StabilityState s;
    qint64 t = 0;
    s = advanceStability(s, Tier::Opus,   Tier::Sonnet, t); t += kTick;  // cand → 1
    EXPECT_EQ(s.ticksStable, 1);
    s = advanceStability(s, Tier::Sonnet, Tier::Sonnet, t); t += kTick;  // revert (held)
    EXPECT_EQ(s.ticksStable, 1) << "tier-lock window must hold accrual";
    s = advanceStability(s, Tier::Sonnet, Tier::Sonnet, t); t += kTick;  // revert (held)
    EXPECT_EQ(s.ticksStable, 1);
    s = advanceStability(s, Tier::Opus,   Tier::Sonnet, t);             // cand → 2
    EXPECT_EQ(s.ticksStable, 2) << "candidate reaches kStableTicks despite noise";
}

// A reversion that OUTLASTS the tier-lock window is a genuine settle-back and
// resets accrual (no false fire). The candidate first appears at t=0; reversions
// continue until past kTierLockWindowMs, after which kStableResetTicks consecutive
// reverts wipe the counter.
TEST(ModelAutoSwitchStability, ReversionBeyondWindowResets) {
    StabilityState s;
    s = advanceStability(s, Tier::Opus, Tier::Sonnet, 0);   // candidate at t=0
    EXPECT_EQ(s.ticksStable, 1);
    // Revert at a time strictly past the lock window, twice (kStableResetTicks).
    const qint64 past = ModelAutoSwitch::kTierLockWindowMs + kTick;
    s = advanceStability(s, Tier::Sonnet, Tier::Sonnet, past);
    s = advanceStability(s, Tier::Sonnet, Tier::Sonnet, past + kTick);
    EXPECT_EQ(s.ticksStable, 0) << "settle-back beyond the window must reset";
    EXPECT_FALSE(s.hasCandidate);
}

// A CHANGED candidate tier restarts accrual from 1 (per-tier accrual) — fixes
// the latent bug where Sonnet→Opus then Sonnet→Haiku ticks both counted toward
// a single switch, firing whichever tier happened to be current on tick 2.
TEST(ModelAutoSwitchStability, ChangedCandidateRestartsAccrual) {
    StabilityState s;
    s = advanceStability(s, Tier::Opus,  Tier::Sonnet, 0);
    EXPECT_EQ(s.ticksStable, 1);
    EXPECT_EQ(s.candidateTier, Tier::Opus);
    // Next tick recommends a DIFFERENT non-current tier — accrual restarts.
    s = advanceStability(s, Tier::Haiku, Tier::Sonnet, kTick);
    EXPECT_EQ(s.ticksStable, 1) << "different candidate must not inherit accrual";
    EXPECT_EQ(s.candidateTier, Tier::Haiku);
}

// ===== ANTS-1944 — reconcileCurrentTier =====
// All tests use the 6-arg signature:
//   reconcileCurrentTier(transcriptTier, fromCommand, transcriptTsMs,
//                         lastActuatedTierName, lastActuatedMs, recommendedTarget)

namespace {
using ModelAutoSwitch::reconcileCurrentTier;
}  // namespace

// INV-3 — fromCommand=true always returns the transcript tier, regardless of
// the actuator record (ANTS-1916 path is authoritative).
TEST(ModelAutoSwitch, Ants1944Inv3CommandAlwaysWins) {
    EXPECT_EQ(reconcileCurrentTier(Tier::Haiku, true, 0,
                                   "opus", 9000000000LL, Tier::Opus),
              Tier::Haiku);
}

// INV-4 — the thrash repro: stale Opus transcript (ts=1000ms), Sonnet actuated
// later (ms=2000), Sonnet re-recommended → anchor returns Sonnet (repeat-suppressed).
TEST(ModelAutoSwitch, Ants1944Inv4ActuatorNewerRepeatSuppressed) {
    EXPECT_EQ(reconcileCurrentTier(Tier::Opus, false, 1000,
                                   "sonnet", 2000, Tier::Sonnet),
              Tier::Sonnet);
}

// INV-5 — missing/unparseable timestamp (tsMs=0) also lets the anchor win on
// a repeat.
TEST(ModelAutoSwitch, Ants1944Inv5MissingTimestampAnchorWins) {
    EXPECT_EQ(reconcileCurrentTier(Tier::Opus, false, 0,
                                   "sonnet", 2000, Tier::Sonnet),
              Tier::Sonnet);
}

// INV-6 — repeat-only: anchor does NOT fire when the clamped recommendation
// differs from the actuated tier. This is the no-user-revert guarantee.
TEST(ModelAutoSwitch, Ants1944Inv6NonRepeatAnchorInert) {
    // Actuated Sonnet, recommending Haiku → NOT a repeat → transcript tier wins.
    EXPECT_EQ(reconcileCurrentTier(Tier::Opus, false, 1000,
                                   "sonnet", 2000, Tier::Haiku),
              Tier::Opus);
}

// INV-7 — transcript newer than actuator → anchor does not override (transcript
// has caught up / user changed it).
TEST(ModelAutoSwitch, Ants1944Inv7TranscriptNewerAnchorInert) {
    EXPECT_EQ(reconcileCurrentTier(Tier::Opus, false, 3000,
                                   "sonnet", 2000, Tier::Sonnet),
              Tier::Opus);
}

// INV-8 — no override when nothing was actuated (empty name), or when the name
// is not a canonical alias (round-trip reject).
TEST(ModelAutoSwitch, Ants1944Inv8NoActuationOrBadAlias) {
    // 8a: empty name → transcript tier (Haiku, distinct from Sonnet recommendation)
    EXPECT_EQ(reconcileCurrentTier(Tier::Haiku, false, 1000,
                                   "", 9000000000LL, Tier::Sonnet),
              Tier::Haiku);
    // 8b: bogus alias → round-trip reject → transcript tier
    EXPECT_EQ(reconcileCurrentTier(Tier::Haiku, false, 1000,
                                   "bogus", 9000000000LL, Tier::Sonnet),
              Tier::Haiku);
    // 8c: model-id shape → round-trip rejects ("claude-opus-4-8"→Opus→"opus" != name)
    //     transcriptTier=Haiku to make the reject observable vs model-id's Opus
    EXPECT_EQ(reconcileCurrentTier(Tier::Haiku, false, 1000,
                                   "claude-opus-4-8", 9000000000LL, Tier::Opus),
              Tier::Haiku);
}

// INV-9 — end-to-end anti-thrash: the reconciled `current` makes decide()
// block the repeat with ONLY target_equals_current (singleton), not an
// unrelated veto. A second row with the unreconciled stale current asserts
// the switch would have fired pre-1944.
TEST(ModelAutoSwitch, Ants1944Inv9EndToEndAntithrash) {
    const Tier reconciledCurrent = reconcileCurrentTier(
        Tier::Opus, false, 1000, "sonnet", 2000, Tier::Sonnet);
    ASSERT_EQ(reconciledCurrent, Tier::Sonnet);  // anchor fired

    // Build a gate where EVERY other guard passes; only target_equals_current
    // should block.
    Gate g;
    g.enabled              = true;
    g.focusedState         = ClaudeState::Idle;
    g.composerEmpty        = true;
    g.recommended          = Tier::Sonnet;   // the re-fire candidate
    g.floor                = Tier::Haiku;
    g.current              = reconciledCurrent;  // Sonnet (anchored)
    g.ticksTargetStable    = ModelAutoSwitch::kStableTicks;
    g.configuredMinDwellMs = 90'000;
    g.msSinceLastSwitch    = 9'000'000LL;    // far above any dwell floor
    g.msSinceLastOverride  = -1;
    g.idleElapsedMs        = -1;
    g.idleCeilingMs        = -1;
    g.composerStaleMs      = -1;
    g.composerStaleThresholdMs = -1;

    const Decision post = ModelAutoSwitch::decide(g);
    EXPECT_FALSE(post.act);
    EXPECT_EQ(post.blockedBy.size(), 1);
    EXPECT_TRUE(post.blockedBy.contains(QStringLiteral("target_equals_current")))
        << "expected sole blocker target_equals_current";

    // Pre-1944 scenario: stale transcript current (Opus) — the switch would fire.
    g.current = Tier::Opus;
    const Decision pre = ModelAutoSwitch::decide(g);
    EXPECT_TRUE(pre.act) << "pre-1944: stale current should have caused a re-fire";
}
