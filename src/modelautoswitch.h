// ANTS-1735 — Autonomous Claude-Code model-switch decision helper (pure).
//
// Shape B of ANTS-1226. This is the actuator's *gate logic*, factored out as a
// pure value-in/value-out function so it is unit-testable without a live
// terminal (the controller in claudestatuswidgets.cpp builds the Gate each tick
// and acts on the Decision). See docs/specs/ANTS-1735.md §2.3 + INV-1..INV-9.
//
// Lives in ants_claude_lib (not ants_core_lib as the spec §4 draft assumed):
// decide() reuses ModelRecommender::tierName for the tier alias (INV-9 forbids
// deriving tierArg any other way), and that symbol lives in ants_claude_lib —
// core placement would invert the core←claude layering. Its only consumer, the
// status-bar controller, is in ants_claude_lib too.
#pragma once

#include <QString>
#include <QtGlobal>

#include "claudeintegration.h"   // ClaudeState
#include "modelrecommender.h"    // ModelRecommender::Tier + tierName

namespace ModelAutoSwitch {

// Gate — every fact decide() needs, snapshotted from the focused tab each tick
// (§2.3). A plain value type so decide() stays pure and table-testable.
struct Gate {
    bool        enabled       = false;                      // config: claude.auto_model_switch
    ClaudeState focusedState  = ClaudeState::NotRunning;    // focused tab's per-shell state (INV-2)
    bool        composerEmpty = true;                       // keystroke-timing proxy (§2.4 / INV-3)
    ModelRecommender::Tier current     = ModelRecommender::Tier::Sonnet;  // tierFromModelId(rec.currentModel)
    ModelRecommender::Tier recommended = ModelRecommender::Tier::Sonnet;  // rec.tier
    ModelRecommender::Tier floor       = ModelRecommender::Tier::Haiku;   // config: claude.auto_model_floor
    int    ticksTargetStable = 0;     // consecutive ticks the CLAMPED target != current (INV-5)
    qint64 msSinceLastSwitch = 0;     // dwell since this tab last switched (INV-6)
};

struct Decision {
    bool    act = false;
    QString tierArg;   // "haiku"|"sonnet"|"opus" — clamped target alias (INV-7)
};

// clampToFloor(rec, floor): a recommendation below the floor returns the floor;
// the floor never constrains an upgrade (Opus is always allowed through). INV-8.
ModelRecommender::Tier clampToFloor(ModelRecommender::Tier rec,
                                    ModelRecommender::Tier floor);

// decide(): act=true only when every gate holds (INV-1..INV-6). On act, tierArg
// is tierName(clampToFloor(recommended,floor)) and nothing else (INV-7/INV-9).
Decision decide(const Gate &g);

constexpr int    kStableTicks = 2;        // ~4 s at the 2 s status tick (INV-5)
constexpr qint64 kMinDwellMs  = 90'000;   // 90 s minimum dwell between switches (INV-6)

}  // namespace ModelAutoSwitch
