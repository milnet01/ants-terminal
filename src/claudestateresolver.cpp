#include "claudestateresolver.h"

#include "terminalwidget.h"

#include <QStringList>

namespace claudestate {

Resolved fromShell(const ClaudeTabTracker::ShellState &s) {
    Resolved r;
    r.base = s.state;
    r.tool = s.tool;
    r.planMode = s.planMode;
    r.auditing = s.auditing;
    r.awaitingInput = s.awaitingInput;
    r.backgroundShells = s.backgroundShells;
    r.backgroundAgents = s.backgroundAgents;
    r.backgroundMonitors = s.backgroundMonitors;
    r.backgroundPending = s.backgroundShells + s.backgroundAgents + s.backgroundMonitors;
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
    // ANTS-5718 — a question with choices, or a plan awaiting approval,
    // blocks on the user while it is the tool in flight. The screen scan
    // (ANTS-1858) lights only the focused tab; this lights every tab.
    if (r.base == ClaudeState::ToolUse
        && (r.tool == QLatin1String("AskUserQuestion")
            || r.tool == QLatin1String("ExitPlanMode")))
        return Display::AwaitingInput;
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
        // ANTS-5773 — Claude's turn is over but it is still waiting on work
        // it started. The overlays above outrank this.
        case ClaudeState::Idle:
            return r.backgroundPending > 0 ? Display::Background : Display::Idle;
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

QString backgroundLabel(const Resolved &r) {
    return QStringLiteral("Claude: waiting (%1 bg)").arg(r.backgroundPending);
}

QString backgroundTooltip(const Resolved &r) {
    QStringList kinds;
    const auto add = [&kinds](int n, const char *one, const char *many) {
        if (n > 0)
            kinds << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
    };
    add(r.backgroundShells, "shell", "shells");
    add(r.backgroundAgents, "agent", "agents");
    add(r.backgroundMonitors, "monitor", "monitors");
    return QStringLiteral("%1 background %2 running: %3")
        .arg(r.backgroundPending)
        .arg(QLatin1String(r.backgroundPending == 1 ? "task" : "tasks"),
             kinds.join(QStringLiteral(", ")));
}

}  // namespace claudestate
