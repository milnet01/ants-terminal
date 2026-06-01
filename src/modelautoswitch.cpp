// ANTS-1735 — see modelautoswitch.h.
// ANTS-1894 — decide() refactored to evaluate every guard and return the full
// blocker list (instead of short-circuiting on the first failure). Bit-for-bit
// preserves the firing-side path; near-miss telemetry consumes blockedBy.
#include "modelautoswitch.h"

#include <algorithm>

namespace ModelAutoSwitch {

namespace {

// ANTS-1894 INV-3 — folded dwell floor. The hard 90 s floor (kMinDwellMs) is
// never undercut even if a degenerate config passes 0.
qint64 effectiveMinDwellMs(const Gate &g) {
    return std::max<qint64>(kMinDwellMs, g.configuredMinDwellMs);
}

}  // namespace

ModelRecommender::Tier clampToFloor(ModelRecommender::Tier rec,
                                    ModelRecommender::Tier floor) {
    // Tier enum is rank-ordered Haiku < Sonnet < Opus. A recommendation below
    // the floor is raised to it; at-or-above passes through (Opus, the top
    // rank, is therefore never clamped down). INV-8.
    return (static_cast<int>(rec) < static_cast<int>(floor)) ? floor : rec;
}

StabilityState advanceStability(const StabilityState     &prev,
                                ModelRecommender::Tier     clampedTarget,
                                ModelRecommender::Tier     current,
                                qint64                     nowMs) {
    StabilityState s = prev;
    if (clampedTarget != current) {
        if (!s.hasCandidate || s.candidateTier != clampedTarget) {
            // New (or changed) candidate tier — start the lock window fresh and
            // begin accrual at 1. Per-tier accrual: a different candidate never
            // inherits the previous candidate's stability.
            s.hasCandidate     = true;
            s.candidateTier    = clampedTarget;
            s.candidateSinceMs = nowMs;
            s.ticksStable      = 1;
        } else {
            // Same candidate as last supporting tick — keep accumulating.
            ++s.ticksStable;
        }
        s.ticksAtCurrent = 0;
    } else {
        // Recommendation clamps back to the current tier this tick.
        ++s.ticksAtCurrent;
        const bool withinLock = s.hasCandidate &&
            (nowMs - s.candidateSinceMs) < kTierLockWindowMs;
        if (!withinLock && s.ticksAtCurrent >= kStableResetTicks) {
            // The reversion has outlasted the tier-lock window (or there was no
            // candidate): a genuine settle-back. Reset accrual (ANTS-1925 rule).
            s.ticksStable  = 0;
            s.hasCandidate = false;
        }
        // Within the window: hold the candidate and its accrued ticksStable —
        // a brief boundary-noise reversion must not wipe a near-ready switch.
    }
    return s;
}

Decision decide(const Gate &g) {
    Decision d;
    d.currentTier     = g.current;
    const ModelRecommender::Tier target = clampToFloor(g.recommended, g.floor);
    d.recommendedTier = target;

    // Evaluate every guard in canonical taxonomy order (INV-2). Each failing
    // guard appends ONE token; reader of blockedBy sees the full diagnostic
    // picture. The seven tokens are the ANTS-1894 INV-9 stable handles — do
    // NOT rename or reorder (consumers persist them to disk).
    if (!g.enabled)
        d.blockedBy << QStringLiteral("auto_switch_disabled");
    // ANTS-1939 — focused_state_not_idle soft-veto. Mirrors the ANTS-1908
    // composer gate: when the agent is busy (state != Idle) but the human
    // hasn't typed for >= composerStaleThresholdMs, treat it as an autonomous
    // window and allow the downgrade. This is exactly when a cheaper model
    // saves the most without interrupting the user. Composes safely with
    // ANTS-1917 idle_end_of_session: that gate only fires when the shell IS
    // idle (idleElapsedMs >= 0); this gate fires when it is NOT idle — the
    // two gates cover complementary, non-overlapping cases.
    // -1 sentinel preserves legacy hard-veto behaviour for callers without
    // keystroke telemetry (bit-for-bit pre-1939 semantics).
    if (g.focusedState != ClaudeState::Idle) {
        const qint64 threshold = (g.composerStaleThresholdMs >= 0)
            ? g.composerStaleThresholdMs
            : kComposerStaleVetoMs;
        const bool humanIdle = (g.composerStaleMs >= 0) &&
                               (g.composerStaleMs >= threshold);
        if (!humanIdle)
            d.blockedBy << QStringLiteral("focused_state_not_idle");
    }
    // ANTS-1908 — composer_not_empty soft-veto. The veto YIELDS when
    // the composer carries text but hasn't been touched within the
    // configurable stale window — this unblocks long autonomous
    // sessions where the dominant blocker is leftover continuation-
    // prompt text sitting idle in the composer. -1 sentinel keeps the
    // legacy hard-veto behaviour for any caller that doesn't supply
    // keystroke telemetry. The blocker token name stays
    // `composer_not_empty` for back-compat with the persisted near-
    // miss ledger; semantics now mean "user is *actively editing*",
    // matching the safety intent.
    // ANTS-1914 — composerStaleThresholdMs is configurable (default
    // kComposerStaleVetoMs ~ 5 min). Advanced users can lower it to
    // unblock slash-command queueing. Full detection of slash-commands
    // requires Claude Code to expose composer text via MCP.
    if (!g.composerEmpty) {
        const qint64 threshold = (g.composerStaleThresholdMs >= 0)
            ? g.composerStaleThresholdMs
            : kComposerStaleVetoMs;
        const bool stale = (g.composerStaleMs >= 0) &&
                           (g.composerStaleMs >= threshold);
        if (!stale) {
            d.blockedBy << QStringLiteral("composer_not_empty");
        }
    }
    if (target == g.current)
        d.blockedBy << QStringLiteral("target_equals_current");
    if (g.ticksTargetStable < kStableTicks)
        d.blockedBy << QStringLiteral("ticks_target_stable_insufficient");
    if (g.msSinceLastSwitch < effectiveMinDwellMs(g))
        d.blockedBy << QStringLiteral("dwell_time_insufficient");
    // ANTS-1890 INV-6 — override cool-down. Sentinel -1 (no override on
    // record or no ledger seen yet) does NOT block; >= 0 within
    // kOverrideCooldownMs does.
    if (g.msSinceLastOverride >= 0 &&
            g.msSinceLastOverride < kOverrideCooldownMs)
        d.blockedBy << QStringLiteral("override_cooldown_active");

    // ANTS-1917 — idle end-of-session suppression (8th token, appended last to
    // preserve the v1 7-token ordering — INV-9). Only fires when the controller
    // supplies a real idle duration (idleElapsedMs >= 0, i.e. the shell IS idle
    // with a known idleSinceMs). A long idle means the session is winding down:
    // a switch would apply to no fresh work and would change the next session's
    // default model. -1 sentinel preserves legacy behaviour for callers that
    // don't supply idle telemetry.
    if (g.idleElapsedMs >= 0) {
        const qint64 ceiling = (g.idleCeilingMs >= 0)
            ? g.idleCeilingMs : kIdleEndOfSessionMs;
        if (g.idleElapsedMs >= ceiling)
            d.blockedBy << QStringLiteral("idle_end_of_session");
    }

    if (d.blockedBy.isEmpty()) {
        d.act     = true;
        d.tierArg = ModelRecommender::tierName(target);    // INV-7 / INV-9 (ANTS-1735)
    }
    return d;
}

double conservatismDwellMultiplier(int  measuredDowngrades,
                                   int  regretRatePct,
                                   int  headlineFloor,
                                   bool isMechanical) {
    // Past calibration: trust is established, no conservatism penalty.
    if (measuredDowngrades >= headlineFloor) return 1.0;
    // Regret within tolerance: the early signal looks fine, stay eager.
    if (regretRatePct <= kRegretConservatismPct) return 1.0;
    // Calibrating with high regret — stretch the dwell. A clearly-mechanical
    // session is a safer downgrade window, so halve the penalty there.
    const double penalty = kCalibrationDwellMult - 1.0;
    const double mult = isMechanical ? (1.0 + penalty * 0.5)
                                     : kCalibrationDwellMult;
    return mult;
}

}  // namespace ModelAutoSwitch
