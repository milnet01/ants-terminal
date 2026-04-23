// Feature-conformance test for spec.md — asserts that plan-mode
// detection reads the correct field name (`permissionMode`) from
// real-world Claude Code JSONL and that `planModeChanged(bool)` fires
// with the right value.
//
// Anchored to the live JSONL schema observed in
// ~/.claude/projects/*/<session>.jsonl as of Claude Code v2.1.87
// (verified 2026-04-23). See spec.md for the canonical event shape.
//
// Links against src/claudeintegration.cpp. Synthesises JSONL transcripts
// on disk and feeds them to ClaudeIntegration::parseTranscriptForState.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeintegration.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>

namespace {

struct Scenario {
    const char *label;
    const char *jsonlContent;
    bool expectPlan;        // final plan-mode state after parse
    bool expectSignal;      // whether planModeChanged should have fired
                            // (only fires on TRANSITION, not same-state re-parse)
};

// Each scenario starts from default plan-mode = false and parses a
// transcript. The expectation column covers the TRANSITION, matching
// the parser's contract: planModeChanged only emits on state change.
//
// Events use the real schema: `{"type":"permission-mode",
// "permissionMode":"<value>","sessionId":"…"}`. The body also includes
// at least one assistant event so parseTranscriptForState has
// something to derive top-level state from — the plan-mode detection
// runs as a sub-scan regardless.
const Scenario kScenarios[] = {
    {
        "enter-plan-mode",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"plan","sessionId":"abc"}
)",
        true, true
    },
    {
        "leave-plan-mode-to-default",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"plan","sessionId":"abc"}
{"type":"permission-mode","permissionMode":"default","sessionId":"abc"}
)",
        false, false  // started false, ended false → no transition
    },
    {
        "accept-edits-is-not-plan",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"acceptEdits","sessionId":"abc"}
)",
        false, false
    },
    {
        "bypass-permissions-is-not-plan",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"bypassPermissions","sessionId":"abc"}
)",
        false, false
    },
    {
        "no-permission-mode-events",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"system","subtype":"turn_duration","ms":123}
)",
        false, false
    },
    {
        "last-toggle-wins-plan-then-plan",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"default","sessionId":"abc"}
{"type":"permission-mode","permissionMode":"plan","sessionId":"abc"}
)",
        true, true
    },
    // Regression guard: the old (pre-0.7.12) code read the field named
    // `mode`. If we regress, a fixture that populates ONLY `mode` (not
    // `permissionMode`) would falsely trigger plan mode. This must
    // NOT fire — the real schema uses `permissionMode`.
    {
        "old-field-name-must-not-trigger",
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","mode":"plan","sessionId":"abc"}
)",
        false, false
    },
};

int runScenarios() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL: could not create temp dir\n");
        return 1;
    }

    int failures = 0;

    for (const auto &s : kScenarios) {
        ClaudeIntegration ci;
        QSignalSpy spy(&ci, &ClaudeIntegration::planModeChanged);

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
        QCoreApplication::processEvents();

        const bool gotPlan = ci.planMode();
        const bool gotSignal = (spy.count() >= 1);
        bool signalArgOk = true;
        if (gotSignal) {
            const QList<QVariant> args = spy.takeFirst();
            signalArgOk = (args.at(0).toBool() == s.expectPlan);
        }

        const bool planOk = (gotPlan == s.expectPlan);
        const bool signalOk = (gotSignal == s.expectSignal) && signalArgOk;
        const bool ok = planOk && signalOk;

        std::fprintf(stderr,
                     "[%-38s] plan=%d (want %d) sig=%d (want %d) argOk=%d  %s\n",
                     s.label, gotPlan, s.expectPlan,
                     gotSignal, s.expectSignal, signalArgOk,
                     ok ? "PASS" : "FAIL");

        if (!ok) ++failures;
    }

    return failures;
}

}  // namespace

// Invariant 4 (from spec.md): "No toggle events → plan mode stays
// whatever it was." The single-parse scenarios can't observe this —
// they start from default planMode=false, parse a toggle-free
// transcript, and assert false, which passes whether state was
// preserved or incorrectly reset.
//
// This scenario runs TWO parses against the same ClaudeIntegration:
//   1. Parse a transcript containing a "plan" toggle → state becomes
//      true.
//   2. Parse a toggle-free transcript — which models the 32 KB-tail
//      window scrolling past an older toggle in a long session.
//      State MUST remain true.
//
// Anchors the "sticky across tail-window drift" behavior that the
// initial `newPlan = false` implementation violated — discovered by
// the /indie-review checkpoint on 2026-04-23.
int runToggleFreeTailPreservesState() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return 1;

    ClaudeIntegration ci;
    QSignalSpy spy(&ci, &ClaudeIntegration::planModeChanged);

    // Phase 1 — seed plan-mode true.
    const QString seedPath = tmp.path() + "/seed.jsonl";
    QFile seed(seedPath);
    if (!seed.open(QIODevice::WriteOnly)) return 1;
    seed.write(
        R"({"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"ok"}]}}
{"type":"permission-mode","permissionMode":"plan","sessionId":"abc"}
)");
    seed.close();
    ci.parseTranscriptForState(seedPath);
    QCoreApplication::processEvents();
    if (!ci.planMode()) {
        std::fprintf(stderr,
            "[invariant4-preserve-toggle-free]  seed phase: "
            "planMode should be true after toggle event, got false  FAIL\n");
        return 1;
    }

    // Phase 2 — parse a transcript with ZERO permission-mode events.
    // Simulates the active tail advancing past the original toggle.
    const QString tailPath = tmp.path() + "/tail.jsonl";
    QFile tail(tailPath);
    if (!tail.open(QIODevice::WriteOnly)) return 1;
    tail.write(
        R"({"type":"user","message":{"content":"please continue"}}
{"type":"assistant","message":{"stop_reason":"tool_use","content":[{"type":"tool_use","name":"Read","input":{}}]}}
{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"x","content":"ok"}]}}
{"type":"assistant","message":{"stop_reason":"end_turn","content":[{"type":"text","text":"done"}]}}
)");
    tail.close();
    ci.parseTranscriptForState(tailPath);
    QCoreApplication::processEvents();

    const bool preserved = ci.planMode();
    std::fprintf(stderr,
        "[invariant4-preserve-toggle-free]  after toggle-free parse: "
        "planMode=%d (want 1)  %s\n",
        preserved, preserved ? "PASS" : "FAIL");

    // Phase 1 flipped NotSet → plan = one emission. Phase 2 observed
    // no permission-mode events, so planMode() stays true without
    // re-emitting. Guards against a regression that would re-fire the
    // same state every parse.
    const long long emissions = static_cast<long long>(spy.count());
    const bool emissionsOk = (emissions == 1);
    std::fprintf(stderr,
        "[invariant4-preserve-toggle-free]  emission count: "
        "planModeChanged fired %lld times (want 1)  %s\n",
        emissions, emissionsOk ? "PASS" : "FAIL");

    return (preserved && emissionsOk) ? 0 : 1;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    int failures = runScenarios();
    failures += runToggleFreeTailPreservesState();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d scenario(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
