// Why this exists: pollClaudeProcess resolved the session transcript
// exactly once per detected PID, after already committing that PID, so a
// resolution that failed — the ordinary case while Claude has started but
// not yet written its first event — was never retried. See spec.md.
//
// Source-grep: the poll reads /proc for a live Claude under a live shell,
// which the unit bundle has neither of.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

namespace {

std::string memberBody(const std::string &src, const char *qualName) {
    const std::string pat =
        std::string("void\\s+") + qualName + R"(\s*\([^)]*\)[^;{]*\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    const size_t start = static_cast<size_t>(m.position(0)) + m.length(0);
    size_t i = start;
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

// The `if (...)` condition that encloses the sessionPathForCwd call:
// scan backwards from the call to the nearest `if (` and return that
// condition's text.
std::string guardOfSessionLookup(const std::string &body) {
    const size_t call = body.find("sessionPathForCwd");
    if (call == std::string::npos) return {};
    const size_t ifPos = body.rfind("if (", call);
    if (ifPos == std::string::npos) return {};
    const size_t open = ifPos + 3;
    size_t i = open + 1;
    int depth = 1;
    while (i < body.size() && depth > 0) {
        if (body[i] == '(') ++depth;
        else if (body[i] == ')') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return body.substr(open, i - open);
}

}  // namespace

TEST(TranscriptResolutionRetry, Main) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string body =
        memberBody(src, "ClaudeIntegration::pollClaudeProcess");
    if (body.empty()) {
        fail("precondition: ClaudeIntegration::pollClaudeProcess body not "
             "found in src/claudeintegration.cpp.");
        ASSERT_EQ(0, failures);
        return;
    }

    // INV-1 — the resolution is retried while no transcript is held.
    const std::string guard = guardOfSessionLookup(body);
    if (guard.empty()) {
        fail("precondition: could not find the `if (...)` guarding the "
             "sessionPathForCwd call.");
    } else if (guard.find("m_transcriptPath") == std::string::npos) {
        std::fprintf(stderr,
            "FAIL: INV-1: the transcript resolution is guarded by `%s`, "
            "which does not test m_transcriptPath. The PID is committed "
            "before the resolution runs, so a resolution that returns "
            "empty — no project dir yet, or Claude started but has not "
            "written its first event — is never retried, and the tab is "
            "left treating every hook as a cold start for the life of the "
            "process. Only a tab switch recovers it.\n",
            guard.c_str());
        ++failures;
    }

    // INV-2 — the backstop still requires a resolved path.
    if (!std::regex_search(
            body, std::regex(R"(!\s*m_transcriptPath\s*\.\s*isEmpty\s*\(\s*\)\s*&&)"))) {
        fail("INV-2: the backstop re-parse no longer requires a resolved "
             "transcript path. Re-parsing an empty path is not the retry "
             "mechanism and must not become one.");
    }

    // INV-3 — Idle stays on the PID change, not on transcript emptiness.
    const size_t idle = body.find("ClaudeState::Idle");
    if (idle == std::string::npos) {
        fail("precondition: the ClaudeState::Idle transition was not found.");
    } else {
        const size_t ifPos = body.rfind("if (", idle);
        const std::string idleGuard =
            (ifPos == std::string::npos) ? std::string()
                                         : body.substr(ifPos, idle - ifPos);
        if (idleGuard.find("m_transcriptPath") != std::string::npos) {
            fail("INV-3: the Idle transition is now reachable from the "
                 "transcript-emptiness condition. It is about a newly "
                 "detected process, and re-emitting it per poll is the "
                 "flapping its existing guard was added to prevent.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/transcript_resolution_retry/spec.md\n",
            failures);
    }
    ASSERT_EQ(0, failures);
}
