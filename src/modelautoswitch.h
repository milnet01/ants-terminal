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
#include <QStringList>
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
    // ANTS-1894 INV-3 — configurable min-dwell floor; the controller sets
    // this from `claude.auto_model_min_dwell_sec`. decide() uses
    // max(kMinDwellMs, configuredMinDwellMs) so the hard 90 s floor is
    // never undercut. Default = kMinDwellMs preserves bit-for-bit pre-1894
    // behaviour for any caller that doesn't set the field.
    qint64 configuredMinDwellMs = 90'000;   // == kMinDwellMs (forward-declared)
    // ANTS-1890 — per-project override cool-down. -1 = no override on record
    // OR no ledger seen yet (cool-down does NOT block); >= 0 = ms since the
    // most-recent project-scoped override of an auto-switch. Sentinel avoids
    // the same-millisecond race that 0 would introduce.
    qint64 msSinceLastOverride = -1;
    // ANTS-1908 — composer_not_empty soft-veto. ms since the user's last
    // keystroke in the focused tab's composer. Read by decide() ONLY when
    // composerEmpty == false: when the composer carries text AND the user
    // hasn't touched it for ≥ kComposerStaleVetoMs, the text is treated as
    // stale (continuation-prompt leftover from a long autonomous session)
    // and the veto YIELDS — no `composer_not_empty` token added. -1 sentinel
    // = no keystroke telemetry / never touched (controller falls back to
    // the legacy hard-veto behaviour, preserving bit-for-bit pre-1908
    // semantics for any caller that doesn't set the field).
    qint64 composerStaleMs = -1;
    // ANTS-1914 — configurable composer-stale threshold. When composerStaleMs >=
    // this value, the guard assumes the text is a leftover (including any queued
    // commands) and yields the veto. Defaults to kComposerStaleVetoMs (~5 min),
    // but advanced users can lower this via config to unblock frequent /model
    // queuing. Note: Full implementation requires Claude Code to expose actual
    // composer text via MCP for precise slash-command detection; this threshold
    // is a user-tunable workaround (see ANTS-1914 for details).
    qint64 composerStaleThresholdMs = -1;  // -1 = use kComposerStaleVetoMs
    // ANTS-1917 — idle end-of-session suppression. ms the focused shell has
    // been sitting in `Idle` (== nowMs - idleSinceMs). A *fresh* between-turns
    // idle is short (Claude just finished a turn, more work is coming); an
    // *end-of-session* idle grows unboundedly (the user walked away). When this
    // exceeds the ceiling, decide() blocks with `idle_end_of_session`: a switch
    // here applies to no fresh work AND silently changes the NEXT session's
    // default model (Claude Code's /model persists the choice). -1 sentinel =
    // not idle / no telemetry → the gate does NOT fire (legacy-safe; the
    // default-constructed Gate keeps the v1 7-token behaviour).
    qint64 idleElapsedMs = -1;
    // ANTS-1917 — configurable idle ceiling. -1 = use kIdleEndOfSessionMs.
    // Controller reads `claude.auto_model_idle_ceiling_sec` (default maps to
    // kIdleEndOfSessionMs; a value <= 0 there disables the gate by leaving
    // idleElapsedMs at -1).
    qint64 idleCeilingMs = -1;
};

struct Decision {
    bool        act = false;
    QString     tierArg;       // "haiku"|"sonnet"|"opus" — clamped target alias (INV-7)
    // ANTS-1894 — populated when act=false; one canonical token per failed
    // guard, in evaluation order. Empty iff act=true. Tokens are the v1
    // taxonomy locked by ANTS-1894 INV-9 — never rename or renumber.
    QStringList blockedBy;
    ModelRecommender::Tier currentTier     = ModelRecommender::Tier::Sonnet;
    ModelRecommender::Tier recommendedTier = ModelRecommender::Tier::Sonnet;
};

// clampToFloor(rec, floor): a recommendation below the floor returns the floor;
// the floor never constrains an upgrade (Opus is always allowed through). INV-8.
ModelRecommender::Tier clampToFloor(ModelRecommender::Tier rec,
                                    ModelRecommender::Tier floor);

// ANTS-1928 — stability-accrual state, advanced once per status tick by the
// controller. Factored out as a pure value type so the tier-lock window logic
// is table-testable without a live terminal. The controller holds one instance
// (window-global, not per-tab) and feeds gate.ticksTargetStable from .ticksStable.
struct StabilityState {
    int    ticksStable      = 0;   // consecutive ticks supporting the candidate (INV-5 gate input)
    int    ticksAtCurrent   = 0;   // consecutive "recommendation clamps to current" ticks (ANTS-1925)
    bool   hasCandidate     = false;
    ModelRecommender::Tier candidateTier = ModelRecommender::Tier::Sonnet;
    qint64 candidateSinceMs = 0;   // wall-clock ms when the current candidate first appeared
};

// advanceStability(prev, clampedTarget, current, nowMs): fold one tick's clamped
// recommendation into the accrual state and return the updated value.
//
// ANTS-1928 tier-lock window. The dominant near-miss blocker
// (`ticks_target_stable_insufficient`, ~67% of blocks) is score oscillation at
// tier boundaries: a recommendation that appears for one tick then reverts to
// `current` for a tick or two never accrues kStableTicks consecutive stable
// ticks. The fix holds a non-current candidate tier for kTierLockWindowMs so a
// brief reversion does NOT reset the accrued stability — the candidate keeps
// accumulating across the noise. A genuine settle-back (reversion that outlasts
// the window) still resets via the ANTS-1925 kStableResetTicks rule. A *changed*
// candidate tier restarts the window fresh (accrual is per-tier, fixing a latent
// bug where Sonnet→Opus then Sonnet→Haiku ticks both counted toward one switch).
StabilityState advanceStability(const StabilityState     &prev,
                                ModelRecommender::Tier     clampedTarget,
                                ModelRecommender::Tier     current,
                                qint64                     nowMs);

// decide(): act=true only when every gate holds (INV-1..INV-6). On act, tierArg
// is tierName(clampToFloor(recommended,floor)) and nothing else (INV-7/INV-9).
Decision decide(const Gate &g);

constexpr int    kStableTicks      = 2;   // ~4 s at the 2 s status tick (INV-5)
// ANTS-1925 — reset-hysteresis. The stable counter resets only after this
// many consecutive ticks where clampedTarget==current. A single noise tick
// at the score boundary no longer wipes a near-ready switch candidate.
constexpr int    kStableResetTicks = 2;   // ~4 s of consistent "already on target"
// ANTS-1928 — tier-lock window. Once a non-current candidate tier appears, a
// brief reversion to current within this window does not reset accrued
// stability. 8 s = 2× the 4 s (kStableTicks × 2 s tick) stability window;
// tolerates oscillation duty cycles down to ~1-in-3 ticks before a candidate
// is abandoned. A reversion that outlasts the window resets normally.
constexpr qint64 kTierLockWindowMs = 8'000;   // 8 s
constexpr qint64 kMinDwellMs  = 90'000;   // 90 s minimum dwell between switches (INV-6)
// ANTS-1890 — separate, stricter floor that applies only when a
// project-scoped override is on record. The dwell rule (kMinDwellMs)
// still applies independently; both must pass.
constexpr qint64 kOverrideCooldownMs = 10 * 60 * 1'000;   // 10 min
// ANTS-1908 — composer_not_empty soft-veto threshold. When the
// composer is non-empty AND no keystroke has landed in this window,
// the gate treats the text as stale (continuation-prompt leftover from
// a long autonomous session) and the veto yields. 5 min picked to
// match the user's "I'm typing something now" vs "I forgot text was
// there" mental model: a real composer edit is updated every few
// seconds, leftover prompts sit idle for many minutes.
constexpr qint64 kComposerStaleVetoMs = 5 * 60 * 1'000;   // 5 min
// ANTS-1917 — idle end-of-session ceiling. When the focused shell has been
// idle longer than this, the session is treated as winding down and the
// switch is suppressed (it would apply to no fresh work and pollute the next
// session's default model). 3 min is generous: it stays well clear of normal
// between-turn gaps and short read-pauses, only tripping on a clear walk-away.
// On a /loop autonomous session the idle gaps stay short (the loop continues),
// so switches fire normally until the task actually ends.
constexpr qint64 kIdleEndOfSessionMs = 3 * 60 * 1'000;    // 3 min

}  // namespace ModelAutoSwitch
