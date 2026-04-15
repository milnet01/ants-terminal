// Feature-conformance test for spec.md — asserts the Claude Code
// status-bar contract:
//   (A) transcript → ClaudeState mapping (every documented last-event type)
//   (B) tab-switch via setShellPid clears stale state immediately
//   (C) context% derived from usage.input_tokens
//
// Links against src/claudeintegration.cpp. Does not spawn a real Claude
// Code process — we synthesise JSONL transcripts on disk and feed them
// to the parser directly.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeintegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>
#include <string>

namespace {

struct Expected {
    const char *label;
    const char *jsonlContent;
    ClaudeState state;
    const char *toolName;   // "" if not applicable (state != ToolUse)
};

// JSONL snippets covering every documented last-event variant. Each ends
// with metadata events where realistic, so the parser's metadata-skip
// logic is exercised (not just "last line wins").
const Expected kScenarios[] = {
    {
        "assistant-tool-use",
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read","input":{"file_path":"/tmp/x"}}]}}
{"type":"system","subtype":"turn_duration","ms":123}
)",
        ClaudeState::ToolUse, "Read"
    },
    {
        "assistant-end-turn",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]}}
{"type":"last-prompt","text":"hello"}
{"type":"file-history-snapshot","files":[]}
)",
        ClaudeState::Idle, ""
    },
    {
        "assistant-streaming-nullstop",
        R"({"type":"assistant","message":{"stop_reason":null,"content":[{"type":"text","text":"still streaming..."}]}}
)",
        ClaudeState::Thinking, ""
    },
    {
        "user-toolresult",
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}}
{"type":"system","subtype":"permission-mode","value":"accept"}
)",
        ClaudeState::Thinking, ""
    },
    {
        "user-plain-prompt",
        R"({"type":"user","message":{"content":"please refactor foo.cpp"}}
{"type":"meta","note":"ignored"}
)",
        ClaudeState::Thinking, ""
    },
    {
        "assistant-max-tokens",
        R"({"type":"assistant","message":{"stop_reason":"max_tokens","content":[{"type":"text","text":"partial response"}]}}
{"type":"summary","text":"chunk"}
)",
        ClaudeState::Idle, ""
    },
};

int runTranscriptScenarios() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL: could not create temp dir\n");
        return 1;
    }

    int failures = 0;

    for (const auto &s : kScenarios) {
        ClaudeIntegration ci;
        QSignalSpy spy(&ci, &ClaudeIntegration::stateChanged);

        const QString path = tmp.path() + "/" + s.label + ".jsonl";
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "FAIL [%s]: could not write transcript\n", s.label);
            ++failures;
            continue;
        }
        f.write(s.jsonlContent);
        f.close();

        ci.parseTranscriptForState(path);

        // Poll Qt's event loop one tick so queued-signal deliveries go through
        // (QSignalSpy works with direct connections, but being explicit avoids
        // flakiness if ClaudeIntegration ever switches to Qt::QueuedConnection).
        QCoreApplication::processEvents();

        ClaudeState got = ci.currentState();
        QString tool = ci.currentTool();

        const bool stateOk = (got == s.state);
        const bool toolOk  = (QString(s.toolName) == tool);

        std::fprintf(stderr,
                     "[%-30s] state=%d (want %d) tool='%s' (want '%s')  %s\n",
                     s.label, (int)got, (int)s.state,
                     qUtf8Printable(tool), s.toolName,
                     (stateOk && toolOk) ? "PASS" : "FAIL");

        if (!stateOk || !toolOk) ++failures;
    }

    return failures;
}

int runTabSwitchStateReset() {
    // Scenario: ClaudeIntegration is watching shell PID 1000 which has
    // Claude running in ToolUse state. User switches to tab whose shell
    // is PID 2000 (no Claude). Status MUST clear immediately — before
    // the next poll tick, not after — or tab A's "Claude: Read..."
    // bleeds into tab B's status bar.
    ClaudeIntegration ci;

    // Seed state by parsing a ToolUse transcript.
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString path = tmp.path() + "/seed.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(
        R"({"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Bash","input":{}}]}})");
    f.close();

    // Pretend we were bound to shell 1000 with Claude active.
    ci.setShellPid(1000);
    ci.parseTranscriptForState(path);

    const ClaudeState pre = ci.currentState();
    const QString preTool = ci.currentTool();

    if (pre != ClaudeState::ToolUse || preTool != "Bash") {
        std::fprintf(stderr,
            "[tab-switch-setup] seed failed: state=%d tool='%s' "
            "(want ToolUse/Bash)  FAIL\n",
            (int)pre, qUtf8Printable(preTool));
        return 1;
    }

    // Now switch to a different shell PID. Expect immediate state reset
    // and a stateChanged(NotRunning, …) emission.
    QSignalSpy spy(&ci, &ClaudeIntegration::stateChanged);
    ci.setShellPid(2000);

    const ClaudeState post = ci.currentState();
    const QString postTool = ci.currentTool();
    const int postCtx = ci.contextPercent();

    const bool stateCleared  = (post == ClaudeState::NotRunning);
    const bool toolCleared   = postTool.isEmpty();
    const bool ctxCleared    = (postCtx == 0);
    const bool signalEmitted = (spy.count() >= 1);
    bool signalArgOk = false;
    if (signalEmitted) {
        const QList<QVariant> args = spy.takeFirst();
        signalArgOk = (args.at(0).value<ClaudeState>() == ClaudeState::NotRunning);
    }

    const bool ok = stateCleared && toolCleared && ctxCleared &&
                    signalEmitted && signalArgOk;
    std::fprintf(stderr,
        "[tab-switch-reset]   state=%d tool='%s' ctx=%d sig=%d sigArg=%d  %s\n",
        (int)post, qUtf8Printable(postTool), postCtx,
        signalEmitted, signalArgOk, ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}

int runContextPercent() {
    // assistant event with usage.input_tokens > 0 → contextUpdated fires
    // with percent = min(100, input_tokens * 100 / 200000).
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;
    const QString path = tmp.path() + "/ctx.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return 1;
    f.write(
        R"({"type":"assistant","message":{"stop_reason":"end_turn","usage":{"input_tokens":50000},"content":[{"type":"text","text":"x"}]}})");
    f.close();

    ClaudeIntegration ci;
    QSignalSpy spy(&ci, &ClaudeIntegration::contextUpdated);
    ci.parseTranscriptForState(path);

    const int got = ci.contextPercent();
    const int want = 25;   // 50000 * 100 / 200000

    const bool ok = (got == want) && (spy.count() >= 1);
    std::fprintf(stderr,
        "[context-percent]    input_tokens=50000 percent=%d (want %d) signals=%d  %s\n",
        got, want, (int)spy.count(), ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    int failures = 0;
    failures += runTranscriptScenarios();
    failures += runTabSwitchStateReset();
    failures += runContextPercent();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d scenario(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
