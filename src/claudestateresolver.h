#pragma once

// ANTS-1873 — Single source of truth for the focused Claude session's
// display state. Adopted by the per-tab dot (ColoredTabBar indicator
// provider) and the bottom status-bar label (ClaudeStatusBarController::
// apply). The autonomous model-switch gate (ANTS-1735 INV-2) may read
// `forFocused(...).base == Idle` once its actuator wiring lands.
//
// Pre-ANTS-1873 the two display surfaces consulted independent state
// caches (tracker per-tab vs cached scalars updated from integration
// signals) and drifted in both directions. Routing every surface through
// this helper makes the agreement structural.

#include "claudeintegration.h"   // ClaudeState
#include "claudetabtracker.h"    // ClaudeTabTracker::ShellState

#include <QString>
#include <sys/types.h>

class TerminalWidget;

namespace claudestate {

struct Resolved {
    ClaudeState base = ClaudeState::NotRunning;
    QString tool;
    bool planMode = false;
    bool auditing = false;
    bool awaitingInput = false;
    // ANTS-5773 — pending background tasks by kind, and their sum.
    int backgroundShells = 0;
    int backgroundAgents = 0;
    int backgroundMonitors = 0;
    int backgroundPending = 0;
};

// Display-precedence ladder shared by every surface. Order:
//   1. awaitingInput        (most user-actionable signal, can fire
//                            before any transcript event lands)
//   2. planMode             (only when base != NotRunning;
//                            collapses to Hidden otherwise — INV-3)
//   3. auditing             (only when base != NotRunning;
//                            collapses to Hidden otherwise — INV-4)
//   4. base                 (Idle / Thinking / ToolUse / Compacting);
//                            Idle with backgroundPending > 0 is
//                            Background (ANTS-5773)
// Returns Hidden when base == NotRunning and no awaitingInput overlay.
enum class Display {
    Hidden,
    AwaitingInput,
    Planning,
    Auditing,
    Idle,
    Thinking,
    ToolUseBash,
    ToolUseGeneric,
    Compacting,
    Background,   // ANTS-5773 — idle, but background work is still running
};

Resolved fromShell(const ClaudeTabTracker::ShellState &s);
Resolved forPid(const ClaudeTabTracker *tracker, pid_t shellPid);
Resolved forFocused(const ClaudeTabTracker *tracker,
                    const TerminalWidget *focused);
Display display(const Resolved &r);

// ANTS-5773 § 2.5 — the Background state's status-bar text,
// "Claude: waiting (N bg)", and its tooltip naming each kind.
QString backgroundLabel(const Resolved &r);
QString backgroundTooltip(const Resolved &r);

}  // namespace claudestate
