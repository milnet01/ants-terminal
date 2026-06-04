#include "claudestateresolver.h"

#include "terminalwidget.h"

namespace claudestate {

Resolved fromShell(const ClaudeTabTracker::ShellState &s) {
    Resolved r;
    r.base = s.state;
    r.tool = s.tool;
    r.planMode = s.planMode;
    r.auditing = s.auditing;
    r.awaitingInput = s.awaitingInput;
    return r;
}

Resolved forPid(const ClaudeTabTracker *tracker, pid_t shellPid) {
    if (!tracker || shellPid <= 0) return {};
    return fromShell(tracker->shellState(shellPid));
}

Resolved forFocused(const ClaudeTabTracker *tracker,
                    const TerminalWidget *focused) {
    if (!tracker || !focused) return {};
    return forPid(tracker, focused->shellPid());
}

Display display(const Resolved &r) {
    if (r.awaitingInput) return Display::AwaitingInput;
    if (r.base == ClaudeState::NotRunning) return Display::Hidden;
    if (r.planMode) return Display::Planning;
    if (r.auditing) return Display::Auditing;
    // ANTS-2009 — deliberately NO `default:` here. With -Wall (-Wswitch) on,
    // an exhaustive defaultless switch makes a newly-added ClaudeState a
    // compile-time warning at THIS site — the strongest "can't silently fall
    // through" guarantee available. A `default:` (even one calling
    // Q_UNREACHABLE()) would suppress that warning and let a new state reach the
    // trailing `return Display::Hidden` unnoticed. Leave it defaultless.
    switch (r.base) {
        case ClaudeState::Idle: return Display::Idle;
        case ClaudeState::Thinking: return Display::Thinking;
        case ClaudeState::ToolUse:
            return r.tool.compare(QLatin1String("Bash"),
                                  Qt::CaseInsensitive) == 0
                ? Display::ToolUseBash
                : Display::ToolUseGeneric;
        case ClaudeState::Compacting: return Display::Compacting;
        case ClaudeState::NotRunning: return Display::Hidden;
    }
    return Display::Hidden;
}

}  // namespace claudestate
