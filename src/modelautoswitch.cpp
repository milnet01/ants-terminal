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

ModelRecommender::Tier reconcileCurrentTier(
    ModelRecommender::Tier transcriptTier,
    bool                   transcriptFromCommand,
    qint64                 transcriptTsMs,
    const QString         &lastActuatedTierName,
    qint64                 lastActuatedMs,
    ModelRecommender::Tier recommendedTarget) {
    // ANTS-1944 — see header. A /model command read is authoritative (ANTS-1916).
    if (transcriptFromCommand) return transcriptTier;
    // Nothing actuated this session → only the transcript knows the model.
    if (lastActuatedTierName.isEmpty()) return transcriptTier;
    // Map the stored alias back to a Tier. tierFromModelId returns Sonnet for any
    // unrecognised string, so a round-trip check rejects a malformed record
    // (defensive — the actuator only ever stores a tierName() output).
    const ModelRecommender::Tier actuated =
        ModelRecommender::tierFromModelId(lastActuatedTierName);
    if (ModelRecommender::tierName(actuated) != lastActuatedTierName)
        return transcriptTier;
    // Repeat-only: anchor ONLY when this would suppress re-firing the tier we
    // already injected. Never steer `current` toward a different tier (so an
    // unlogged user /model to another tier is never reverted by the anchor).
    if (actuated != recommendedTarget) return transcriptTier;
    // The actuator's switch is newer than the (stale) assistant-turn read →
    // trust what we actually set. transcriptTsMs==0 (missing/unparseable) counts
    // as oldest, so the anchor wins — the intended degradation.
    if (lastActuatedMs > transcriptTsMs) return actuated;
    return transcriptTier;
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
    // picture. The tokens are the ANTS-1894 INV-9 stable handles — do NOT
    // rename or reorder (consumers persist them to disk). v1 = 7 tokens;
    // ANTS-1917 appended idle_end_of_session (8th); ANTS-1959 appended
    // downgrade_requires_active_work (9th). New tokens always go last.
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
    // ANTS-1959 — long-ToolUse window: a Bash/tool call running for
    // >= kLongToolUseMs means the main loop is I/O-bound and human-idle
    // (a build/test wait). Computed once here and shared by the
    // focused-state gate (ANTS-1959) and the composer gate (ANTS-1973).
    // Sentinel -1 (no tool-use telemetry) preserves v1 behaviour.
    const bool longToolUse = (g.toolUseElapsedMs >= 0) &&
                             (g.toolUseElapsedMs >= kLongToolUseMs);
    if (g.focusedState != ClaudeState::Idle) {
        const qint64 threshold = (g.composerStaleThresholdMs >= 0)
            ? g.composerStaleThresholdMs
            : kComposerStaleVetoMs;
        const bool humanIdle = (g.composerStaleMs >= 0) &&
                               (g.composerStaleMs >= threshold);
        // Safe to downgrade regardless of composer state during a long
        // ToolUse: the user is waiting on the command, not driving the agent.
        if (!humanIdle && !longToolUse)
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
    // ANTS-1973 — yield additionally during a long foreground ToolUse
    // wait. composer_not_empty is the dominant near-miss blocker (Vestige:
    // 67%). The 5-min stale window above is far too long for an ~18 s
    // build wait, so when a command has been running >= kLongToolUseMs AND
    // the composer has not been touched for >= kComposerStaleDuringToolUseMs
    // (a SHORT threshold), the staged text is queued/abandoned — not active
    // typing — and the veto yields. Needs only composerStaleMs +
    // toolUseElapsedMs, both already on the Gate (no composer-content
    // visibility required; ANTS-1931). Sentinel -1 preserves v1 behaviour.
    if (!g.composerEmpty) {
        const qint64 threshold = (g.composerStaleThresholdMs >= 0)
            ? g.composerStaleThresholdMs
            : kComposerStaleVetoMs;
        const bool stale = (g.composerStaleMs >= 0) &&
                           (g.composerStaleMs >= threshold);
        const bool staleDuringToolUse = longToolUse &&
            (g.composerStaleMs >= 0) &&
            (g.composerStaleMs >= kComposerStaleDuringToolUseMs);
        if (!stale && !staleDuringToolUse) {
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

    // ANTS-1959 — downgrades are unsafe while the shell is idle (9th token,
    // appended last to preserve the v1 8-token ordering — INV-9). A downgrade
    // at idle — end of work, between turns, winding down — is ALL-RISK and
    // little reward: it can pollute the next session's default model, it
    // precedes a reasoning burst as often as not, and (the user's key concern)
    // when paired with any continuation it can start BILLABLE work that wasn't
    // asked for. Meanwhile the cheaper tier isn't even exercised while idle —
    // the recommender re-scores the tier on the next active turn anyway, so a
    // genuinely-mechanical stretch still gets its downgrade then (during the
    // ToolUse grind, where focused_state_not_idle yields — Signal 1). So we
    // suppress downgrades whenever the shell IS idle and permit them only
    // during active work. Keyed on idleElapsedMs >= 0 (real idle telemetry,
    // mirroring idle_end_of_session); the -1 sentinel keeps legacy/no-telemetry
    // callers bit-for-bit. UPGRADES are unrestricted — you always want Opus the
    // moment work turns hard. Tier enum is Haiku<Sonnet<Opus, so a smaller rank
    // == a cheaper tier == a downgrade.
    const bool isDowngrade =
        static_cast<int>(target) < static_cast<int>(g.current);
    if (isDowngrade && g.idleElapsedMs >= 0)
        d.blockedBy << QStringLiteral("downgrade_requires_active_work");

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

bool switchConfirmVisible(const QString &recentOutput) {
    // Title + at least one option line. The title alone is specific, but
    // pairing it with an option keeps a stray mention of the phrase in
    // scrollback prose (or this very source quoted in a transcript) from
    // tripping a false confirm. Strings verified against live CC 2026-06-01.
    const bool title  = recentOutput.contains(
        QStringLiteral("Switch model?"), Qt::CaseInsensitive);
    const bool option = recentOutput.contains(
            QStringLiteral("Yes, switch to"), Qt::CaseInsensitive)
        || recentOutput.contains(
            QStringLiteral("No, go back"), Qt::CaseInsensitive);
    return title && option;
}

bool shouldAutoConfirmUnarmedSwitch(bool dialogVisible,
                                    bool enabled,
                                    bool handshakeInFlight,
                                    bool alreadyConfirmed) {
    // An Ants-initiated handshake (auto-switch / chip-click / undo) already
    // polls and confirms its own dialog — never double-press over it.
    return dialogVisible && enabled && !handshakeInFlight && !alreadyConfirmed;
}

bool shouldContinueAfterUnarmedConfirm(bool autoModeOn, bool activeTurn) {
    // Drive the workflow forward only when the user opted into auto mode AND a
    // turn is still active to resume (idle ⇒ no continuation, ANTS-1959).
    return autoModeOn && activeTurn;
}

// ANTS-1975 — when the user types `/model <tier>` with an explicit arg CC
// switches directly (no "Switch model?" dialog) and prints a one-liner
// "Set model to <Tier> <ver> and saved as your default for new sessions".
// ANTS-2020 — anchor the title phrase to a known tier token immediately
// following it, the same title+token structure switchConfirmVisible uses. The
// bare "Set model to" substring tripped on build output / a log line / a
// transcript quoting the phrase, which (with auto-mode on) fired an unwanted
// continuation via shouldContinueAfterDirectSwitch. A future tier name is a
// false-negative (no spurious continuation — the safe direction), never a
// false-positive. Tier set verified against the model_switch_confirm
// feature-test banners (Sonnet 4.6 / Opus 4.8).
bool directModelSwitchVisible(const QString &recentOutput) {
    return recentOutput.contains(
               QStringLiteral("Set model to Sonnet"), Qt::CaseInsensitive)
        || recentOutput.contains(
               QStringLiteral("Set model to Opus"), Qt::CaseInsensitive)
        || recentOutput.contains(
               QStringLiteral("Set model to Haiku"), Qt::CaseInsensitive)
        || recentOutput.contains(
               QStringLiteral("Set model to Default"), Qt::CaseInsensitive);
}

// ANTS-1975 — for the no-dialog direct-switch path: resume iff auto mode on.
// Unlike the dialog path (ANTS-1969) we do NOT gate on activeTurn — the user
// explicitly typed /model to change model, which IS the "request" for fresh
// work (it is not an Ants-initiated switch at idle). The billing-safety concern
// of ANTS-1959 applies to Ants-initiated idle switches; a deliberate user
// command is a different context. If the session was at idle because context
// compacted, this gets work going again — which is what the user expects.
bool shouldContinueAfterDirectSwitch(bool autoModeOn) {
    return autoModeOn;
}

}  // namespace ModelAutoSwitch
