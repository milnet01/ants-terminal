// Feature-conformance test for ANTS-1225 — claudeintegration::pollClaudeProcess
// must rebind transcript and emit stateChanged(Idle) when the focused tab's
// Claude PID changes (initial detection OR live replacement after /exit +
// claude --resume within the 2 s poll window). See spec.md.
//
// INV labels qualified ANTS-1225-INV-N. Anchored by `// ANTS-1225-INV-N`
// comments in src/claudeintegration.cpp next to each load-bearing string.
//
// Test shape (source-grep, no GUI, no QTemporaryDir, no instantiation):
//  * Slurp src/claudeintegration.cpp.
//  * For each INV, assert (a) the anchor comment is present and (b) the
//    load-bearing string sits inside pollClaudeProcess via a brace-walk
//    locality check.
//
// Why this exists: lock the rebind-on-PID-change contract so a future
// refactor that drops the `!= foundPid` gate, the watcher-swap sequence,
// the not-found clear, or the m_shellPid early-out surfaces as a red
// build (specific INV violation) rather than the user-visible "Claude
// status indicator hidden until tab-switch" symptom that triggered
// ANTS-1225.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {




// Return the substring of `src` from the first occurrence of `sigPrefix`
// up to the matching `}` that closes the function body. Falls back to a
// generous slice if the brace walk never returns to depth 0.
std::string functionBody(const std::string &src,
                         const std::string &sigPrefix) {
    const auto start = src.find(sigPrefix);
    if (start == std::string::npos) return {};
    auto open = src.find('{', start);
    if (open == std::string::npos) return src.substr(start, 4096);
    int depth = 0;
    size_t i = open;
    for (; i < src.size(); ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') {
            --depth;
            if (depth == 0) { ++i; break; }
        }
    }
    return src.substr(start, i - start);
}

bool bodyContains(const std::string &src,
                  const std::string &sigPrefix,
                  const std::string &needle) {
    const std::string body = functionBody(src, sigPrefix);
    return body.find(needle) != std::string::npos;
}

void testInv1ReplacementGate(const std::string &claudeintegration) {
    // INV-1: the rebind branch's gate must be `m_claudePid != foundPid`,
    // not `m_claudePid == 0`. The latter only catches initial detection;
    // the former catches both initial detection AND live replacement
    // (the bug ANTS-1225 fixes). Locality matters: the anchor and gate
    // must live inside pollClaudeProcess.
    const std::string sig =
        "void ClaudeIntegration::pollClaudeProcess(";

    expect(bodyContains(claudeintegration, sig, "// ANTS-1225-INV-1"),
           "ANTS-1225-INV-1: anchor comment present in pollClaudeProcess");
    expect(bodyContains(claudeintegration, sig,
                        "if (m_claudePid != foundPid)"),
           "ANTS-1225-INV-1: rebind gate is `m_claudePid != foundPid` "
           "(handles both initial detection and live PID replacement)");
    // Defensive: the pre-fix gate `if (m_claudePid == 0)` MUST NOT be
    // present inside pollClaudeProcess — it would either replace INV-1
    // (regression) or co-exist as a redundant early-return that masks
    // the new gate.
    expect(!bodyContains(claudeintegration, sig,
                         "if (m_claudePid == 0)"),
           "ANTS-1225-INV-1: pre-fix `m_claudePid == 0` gate is absent "
           "from pollClaudeProcess");
}

void testInv2RebindSequence(const std::string &claudeintegration) {
    // INV-2: when the rebind branch fires, the load-bearing sequence is
    // (in order) m_transcriptPath = scoped → removePaths → addPath →
    // parseTranscriptForState → emit stateChanged(m_state, "idle").
    //
    // The strings `parseTranscriptForState(m_transcriptPath)` (3 sites
    // in this file) and `emit stateChanged(m_state, "idle")` (2 sites)
    // are NOT unique in claudeintegration.cpp — that's why the spec
    // mandates the `// ANTS-1225-INV-2` anchor: the source-grep test
    // must verify the strings are co-located with the anchor INSIDE
    // pollClaudeProcess, not in the constructor (line ~37) or the
    // backstop re-parse (line ~277) or another emission path
    // (line ~1018).
    const std::string sig =
        "void ClaudeIntegration::pollClaudeProcess(";

    expect(bodyContains(claudeintegration, sig, "// ANTS-1225-INV-2"),
           "ANTS-1225-INV-2: anchor comment present in pollClaudeProcess");
    expect(bodyContains(claudeintegration, sig,
                        "m_transcriptWatcher.addPath(m_transcriptPath)"),
           "ANTS-1225-INV-2: addPath rebind in pollClaudeProcess body");
    expect(bodyContains(claudeintegration, sig,
                        "m_transcriptWatcher.removePaths"),
           "ANTS-1225-INV-2: removePaths in pollClaudeProcess body");
    expect(bodyContains(claudeintegration, sig,
                        "parseTranscriptForState(m_transcriptPath)"),
           "ANTS-1225-INV-2: parseTranscriptForState seeds state from "
           "the rebound transcript");
    expect(bodyContains(claudeintegration, sig,
                        "emit stateChanged(m_state, \"idle\")"),
           "ANTS-1225-INV-2: stateChanged(Idle) emitted in pollClaudeProcess");
    expect(bodyContains(claudeintegration, sig,
                        "if (m_state != ClaudeState::Idle)"),
           "ANTS-1225-INV-2: emission gated by `m_state != Idle` "
           "(no flap on steady-state Idle)");
}

void testInv3NotFoundBranch(const std::string &claudeintegration) {
    // INV-3: when findClaudeChildPid returns 0 (no Claude under the
    // focused shell), the existing not-found branch clears m_claudePid,
    // m_transcriptPath, and emits stateChanged(NotRunning). INV-1 does
    // NOT subsume this — both branches coexist.
    const std::string sig =
        "void ClaudeIntegration::pollClaudeProcess(";

    expect(bodyContains(claudeintegration, sig, "// ANTS-1225-INV-3"),
           "ANTS-1225-INV-3: anchor comment present in pollClaudeProcess");
    expect(bodyContains(claudeintegration, sig, "if (!found)"),
           "ANTS-1225-INV-3: not-found branch present");
    expect(bodyContains(claudeintegration, sig, "m_claudePid = 0;"),
           "ANTS-1225-INV-3: not-found branch clears m_claudePid");
    expect(bodyContains(claudeintegration, sig, "m_transcriptPath.clear()"),
           "ANTS-1225-INV-3: not-found branch clears m_transcriptPath");
    expect(bodyContains(claudeintegration, sig,
                        "emit stateChanged(m_state, m_currentTool)"),
           "ANTS-1225-INV-3: not-found branch emits stateChanged(NotRunning)");
}

void testInv4ShellPidEarlyReturn(const std::string &claudeintegration) {
    // INV-4: pollClaudeProcess gates everything on m_shellPid > 0.
    // Without this, findClaudeChildPid(0) would walk /proc/0 which is
    // meaningless. The early-return is the *first* statement of the
    // function body.
    const std::string sig =
        "void ClaudeIntegration::pollClaudeProcess(";

    expect(bodyContains(claudeintegration, sig, "// ANTS-1225-INV-4"),
           "ANTS-1225-INV-4: anchor comment present in pollClaudeProcess");
    expect(bodyContains(claudeintegration, sig, "if (m_shellPid <= 0) return;"),
           "ANTS-1225-INV-4: m_shellPid <= 0 early-return gates the function");
}

}  // namespace

TEST(claude_pid_replacement, Inv1ReplacementGate) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const int before = expect_failures();
    testInv1ReplacementGate(ci);
    if (expect_failures() > before) FAIL();
}

TEST(claude_pid_replacement, Inv2RebindSequence) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const int before = expect_failures();
    testInv2RebindSequence(ci);
    if (expect_failures() > before) FAIL();
}

TEST(claude_pid_replacement, Inv3NotFoundBranch) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const int before = expect_failures();
    testInv3NotFoundBranch(ci);
    if (expect_failures() > before) FAIL();
}

TEST(claude_pid_replacement, Inv4ShellPidEarlyReturn) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const int before = expect_failures();
    testInv4ShellPidEarlyReturn(ci);
    if (expect_failures() > before) FAIL();
}
