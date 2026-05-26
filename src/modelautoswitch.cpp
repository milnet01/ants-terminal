// ANTS-1735 — see modelautoswitch.h.
#include "modelautoswitch.h"

namespace ModelAutoSwitch {

ModelRecommender::Tier clampToFloor(ModelRecommender::Tier rec,
                                    ModelRecommender::Tier floor) {
    // Tier enum is rank-ordered Haiku < Sonnet < Opus. A recommendation below
    // the floor is raised to it; at-or-above passes through (Opus, the top
    // rank, is therefore never clamped down). INV-8.
    return (static_cast<int>(rec) < static_cast<int>(floor)) ? floor : rec;
}

Decision decide(const Gate &g) {
    Decision d;
    if (!g.enabled) return d;                                 // INV-1
    if (g.focusedState != ClaudeState::Idle) return d;        // INV-2
    if (!g.composerEmpty) return d;                           // INV-3

    // Hysteresis + dwell are evaluated against the CLAMPED target, so a
    // recommendation that clamps to the current tier is a no-op and never
    // accumulates stability (no livelock). INV-4 / INV-5.
    const ModelRecommender::Tier target = clampToFloor(g.recommended, g.floor);
    if (target == g.current) return d;                        // INV-4
    if (g.ticksTargetStable < kStableTicks) return d;         // INV-5
    if (g.msSinceLastSwitch < kMinDwellMs) return d;          // INV-6

    d.act     = true;
    d.tierArg = ModelRecommender::tierName(target);           // INV-7 / INV-9
    return d;
}

}  // namespace ModelAutoSwitch
