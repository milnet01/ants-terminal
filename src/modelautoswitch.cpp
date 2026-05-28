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
    if (g.focusedState != ClaudeState::Idle)
        d.blockedBy << QStringLiteral("focused_state_not_idle");
    // ANTS-1908 — composer_not_empty soft-veto. The veto YIELDS when
    // the composer carries text but hasn't been touched within the
    // kComposerStaleVetoMs window — this unblocks long autonomous
    // sessions where the dominant blocker is leftover continuation-
    // prompt text sitting idle in the composer. -1 sentinel keeps the
    // legacy hard-veto behaviour for any caller that doesn't supply
    // keystroke telemetry. The blocker token name stays
    // `composer_not_empty` for back-compat with the persisted near-
    // miss ledger; semantics now mean "user is *actively editing*",
    // matching the safety intent.
    if (!g.composerEmpty) {
        const bool stale = (g.composerStaleMs >= 0) &&
                           (g.composerStaleMs >= kComposerStaleVetoMs);
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

    if (d.blockedBy.isEmpty()) {
        d.act     = true;
        d.tierArg = ModelRecommender::tierName(target);    // INV-7 / INV-9 (ANTS-1735)
    }
    return d;
}

}  // namespace ModelAutoSwitch
