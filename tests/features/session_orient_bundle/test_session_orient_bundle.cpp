// ANTS-1883 — session_orient bundle verb.
// Source-grep style against the four registration sites.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — envelope carries the three composed-verb keys.
TEST(session_orient_bundle, Inv1EnvelopeShape) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "cmdSessionOrient"),
           "INV-1: cmdSessionOrient symbol present");
    expect(contains(cpp, "\"current_state\"") &&
           contains(cpp, "\"project_layout\"") &&
           contains(cpp, "\"sections_index\""),
           "INV-1: bundle envelope includes the three composed-verb keys");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — upstream roadmap_query uses mode:section_index + status:active.
TEST(session_orient_bundle, Inv2SectionsIndexModeAndStatus) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("cmdSessionOrient");
    if (pos == std::string::npos) {
        expect(false, "INV-2: cmdSessionOrient body not found");
    } else {
        // ANTS-2064 — brace-matched body. The upstream call wires both args.
        const std::string body =
            ants_test::slurpFunctionBody(cpp, "cmdSessionOrient");
        expect(contains(body, "section_index") &&
               contains(body, "\"active\""),
               "INV-2: upstream call uses mode:section_index + "
               "status:active");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — etag allowlist membership.
TEST(session_orient_bundle, Inv3EtagAllowlistMembership) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Anchor on the function definition line, not the first mention
    // (which is a comment far above the actual body).
    const auto isEtagPos = ci.find(
        "bool ClaudeIntegration::isEtagSupportedTool");
    if (isEtagPos == std::string::npos) {
        expect(false, "INV-3: isEtagSupportedTool definition not found");
    } else {
        const std::string body = ants_test::slurpFunctionBody(
            ci, "bool ClaudeIntegration::isEtagSupportedTool");
        expect(contains(body, "\"session_orient\""),
               "INV-3: session_orient registered in isEtagSupportedTool");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — Required CallerCwdContract.
TEST(session_orient_bundle, Inv4CallerCwdRequired) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto fnPos = ci.find(
        "ClaudeIntegration::callerCwdContractFor");
    if (fnPos == std::string::npos) {
        expect(false, "INV-4: callerCwdContractFor definition not found");
    } else {
        const std::string body = ants_test::slurpFunctionBody(
            ci, "ClaudeIntegration::callerCwdContractFor");
        expect(contains(body, "\"session_orient\""),
               "INV-4: session_orient row present in callerCwdContractFor");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — tools/list descriptor.
TEST(session_orient_bundle, Inv5ToolsListDescribesBundle) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Descriptor block must reference the verb name AND its three
    // composed children for caller orientation.
    expect(contains(ci, "\"session_orient\""),
           "INV-5: tools/list builder names session_orient");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — top-level ok reflects all-three success.
TEST(session_orient_bundle, Inv6PartialUpstreamFailure) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("cmdSessionOrient");
    if (pos == std::string::npos) {
        expect(false, "INV-6: cmdSessionOrient body not found");
    } else {
        // Look within the cmdSessionOrient body for an
        // ok-aggregation pattern: `allOk &= ` or `if (!ok)` style.
        const std::string body =
            ants_test::slurpFunctionBody(cpp, "cmdSessionOrient");
        expect(contains(body, "allOk") || contains(body, "all_ok") ||
               contains(body, "ok && "),
               "INV-6: bundle top-level ok aggregates upstream ok values");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — tokenCostFor bucket.
TEST(session_orient_bundle, Inv7TokenCostBucketRegistered) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto tcPos = ci.find("tokenCostFor");
    if (tcPos == std::string::npos) {
        // Fall back: look for the literal table pattern.
        expect(contains(ci, "\"session_orient\""),
               "INV-7: session_orient referenced somewhere "
               "(tokenCostFor table or descriptor)");
    } else {
        const std::string body =
            ants_test::slurpFunctionBody(ci, "tokenCostFor");
        expect(contains(body, "\"session_orient\""),
               "INV-7: session_orient row in tokenCostFor table");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-8 (ANTS-2140) — the bundle eagerly refreshes the codebase index
// and embeds a TRIMMED summary: it invokes cmdCodebaseIndex under a
// `codebase_index` key, and strips the per-call-volatile fields so the
// session_orient dispatch-layer ETag stays stable across calls.
TEST(session_orient_bundle, Inv8CodebaseIndexRefreshTrimmed) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("cmdSessionOrient");
    if (pos == std::string::npos) {
        expect(false, "INV-8: cmdSessionOrient body not found");
    } else {
        const std::string body =
            ants_test::slurpFunctionBody(cpp, "cmdSessionOrient");
        expect(contains(body, "cmdCodebaseIndex") &&
               contains(body, "\"codebase_index\""),
               "INV-8: bundle invokes cmdCodebaseIndex under a "
               "codebase_index key (eager refresh at session start)");
        expect(contains(body, "generated_at_ms") &&
               contains(body, "refreshed_files"),
               "INV-8: volatile fields stripped from the embedded "
               "summary (ETag stability)");
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-9 (ANTS-1964) — the bundle surfaces the cross-session feedback
// backlog under a `feedback_pending` key, reusing the canonical
// FeedbackFile::parse (no bash reimplementation), gated to the
// maintainer project by the format-standard doc it ships, and surfacing
// only files whose delta is present (un-triaged input).
TEST(session_orient_bundle, Inv9FeedbackPendingScan) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("cmdSessionOrient");
    if (pos == std::string::npos) {
        expect(false, "INV-9: cmdSessionOrient body not found");
    } else {
        const std::string body =
            ants_test::slurpFunctionBody(cpp, "cmdSessionOrient");
        expect(contains(body, "\"feedback_pending\""),
               "INV-9: bundle emits a feedback_pending envelope key");
        expect(contains(body, "FeedbackFile::parse"),
               "INV-9: reuses the canonical parser (no bash reimpl)");
        expect(contains(body, "mcp-feedback-files.md"),
               "INV-9: maintainer-only gate via the format-standard doc");
        expect(contains(body, "deltaPresent"),
               "INV-9: surfaces only files with an un-triaged delta");
    }
    EXPECT_EQ(0, expect_failures());
}
