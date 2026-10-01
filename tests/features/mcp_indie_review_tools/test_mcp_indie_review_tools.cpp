// Feature-conformance test for ANTS-1112 MCP wiring. Source-grep
// approach: read claudeintegration.cpp + mainwindow.cpp + remotecontrol.h/.cpp
// and verify all 5 tool names are registered in each layer.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {


// ANTS-5485 removed indie_review_brief, _synthesis_prompt, _fold_in and
// _orchestrate.
constexpr const char *kToolNames[2] = {
    "indie_review_partition",
    "indie_review_corroborate",
};

constexpr const char *kCmdMethods[2] = {
    "cmdIndieReviewPartition",
    "cmdIndieReviewCorroborate",
};

}  // namespace

TEST(McpIndieReviewTools, Inv9AllToolNamesInToolsList) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *name : kToolNames) {
        const std::string nameQuoted = std::string("\"") + name + "\"";
        EXPECT_NE(ci.find(nameQuoted), std::string::npos)
            << "tool name " << name << " missing from claudeintegration.cpp";
    }
}

TEST(McpIndieReviewTools, AllProvidersRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpMainWindow();
    ASSERT_FALSE(mw.empty());
    for (const char *name : kToolNames) {
        const std::string call =
            std::string("registerToolProvider(\"") + name + "\"";
        EXPECT_NE(mw.find(call), std::string::npos)
            << "registerToolProvider(\"" << name
            << "\", ...) missing from mainwindow.cpp";
    }
}

TEST(McpIndieReviewTools, AllCmdMethodsDeclaredInHeader) {
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    for (const char *m : kCmdMethods) {
        EXPECT_NE(rch.find(m), std::string::npos)
            << "method " << m << " missing from remotecontrol.h";
    }
}

TEST(McpIndieReviewTools, AllCmdMethodsDefinedInCpp) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    for (const char *m : kCmdMethods) {
        const std::string defn = std::string("RemoteControl::") + m;
        EXPECT_NE(rc.find(defn), std::string::npos)
            << "definition RemoteControl::" << m
            << " missing from remotecontrol.cpp";
    }
}


// ANTS-1288 — the partition handler emits suggested_merges, computed via
// the engine helper (locks the wiring against accidental removal).
TEST(McpIndieReviewTools, Ants1288PartitionEmitsSuggestedMerges) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    // ANTS-4804 — was a fixed 4000-byte scrape window, itself widened from
    // 2000 by ANTS-3709 for the same reason: the handler grew again (per-lane
    // total_lines and the oversized signal) and pushed suggested_merges past
    // the window, reddening a test that asserts nothing about either. Widening
    // a third time would buy the same failure at the next addition — a fixed
    // window measures the handler's LENGTH, not the wiring it claims to lock,
    // as this test's own note already said. slurpFunctionBody brace-matches
    // the real body, which is the repair the sibling INV-5 case made for this
    // exact failure under ANTS-4100.
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdIndieReviewPartition");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("suggested_merges"), std::string::npos)
        << "cmdIndieReviewPartition no longer emits suggested_merges";
    EXPECT_NE(body.find("IndieReviewEngine::suggestedMerges"),
              std::string::npos)
        << "cmdIndieReviewPartition no longer calls the engine helper";
}


// ANTS-3713 — indie_review_corroborate accepts an absolute reports_dir under
// allow_outside_project, reusing test_audit_synthesis_prompt's opt-in name
// (ANTS-1455) rather than inventing a second one. Scoped to this verb's own
// descriptor via mcpToolDescriptor, because the sibling verb declares the
// same property and a whole-file grep would false-green.
TEST(McpIndieReviewTools, Ants3713CorroborateAllowsOutsideProject) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string desc =
        ants_test::mcpToolDescriptor(ci, "indie_review_corroborate");
    ASSERT_FALSE(desc.empty())
        << "indie_review_corroborate descriptor not found in the tools list";
    EXPECT_NE(desc.find("props[\"allow_outside_project\"]"), std::string::npos)
        << "allow_outside_project is not declared on this verb's schema, so "
           "additionalProperties:false rejects it";

    // Handler side: the flag relaxes the anchor AND routes to the
    // already-anchored engine entry point, so ANTS-1282 INV-3 still guards
    // the default path.
    const std::string rc = ants_test::slurpRemoteControl();
    EXPECT_NE(rc.find("/*allowOutsideRoot=*/allowOutside"), std::string::npos);
    EXPECT_NE(rc.find("corroboratedFindingsFromCanonicalDir"),
              std::string::npos);
}

// INV-14 (ANTS-1581 reversal) — the blanket "the skill does not call this
// tool" note is gone from BOTH families, and the one surviving warning is
// scoped to indie_review_dispatch, whose difference is the reviewer it runs
// on rather than who calls it.
TEST(McpIndieReviewTools, Inv14ParallelApiNoteReversedToDispatchOnly) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    // The old note steered readers away from every cold_eyes_ /
    // indie_review_ verb — the opposite of the standing rule that the MCP
    // verbs are the default path. No trace of it may remain, or some verbs
    // still carry it and the reversal is half-applied.
    EXPECT_EQ(ci.find("Parallel API:"), std::string::npos)
        << "INV-14: the ANTS-1581(b) 'Parallel API' note must be gone, not "
           "narrowed further";
    // The surviving carve-out, and its guard.
    const auto note = ci.find("Weaker reviewer:");
    ASSERT_NE(note, std::string::npos)
        << "INV-14: indie_review_dispatch keeps a note saying WHY it differs";
    const auto guard =
        ci.find("name == QLatin1String(\"indie_review_dispatch\")");
    ASSERT_NE(guard, std::string::npos)
        << "INV-14: the note is name-scoped, not prefix-matched over a family";
    EXPECT_LT(guard, note)
        << "INV-14: the guard must precede the note it gates";
}

// INV-15 (ANTS-1581 reversal) — the catalog hint is the other place a caller
// picks a verb from, so it must not present the local-endpoint verb as the
// entry point for a review.
TEST(McpIndieReviewTools, Inv15DispatchSelectionHintNamesTheWeakerReviewer) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const std::string hint =
        ants_test::mcpToolDescriptor(ci, "indie_review_dispatch");
    ASSERT_FALSE(hint.empty());
    EXPECT_NE(hint.find("LOCAL AI endpoint"), std::string::npos)
        << "INV-15: the hint must say where the review actually runs";
    EXPECT_EQ(hint.find("entry-point orchestrator"), std::string::npos)
        << "INV-15: dispatch is not the entry point for a Claude review";
}

TEST(McpIndieReviewTools, AllSchemasUseAdditionalPropertiesFalse) {
    // Defensive: every new tool's inputSchema sets additionalProperties=false
    // so unknown keys are rejected. Region scoped to JUST the indie_review_*
    // block — start at indie_review_partition, end at the next non-indie
    // tool block (verify_changes, ANTS-1289).
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("\"indie_review_partition\"");
    ASSERT_NE(block_start, std::string::npos);
    // End: the next tool series after indie_review_dispatch.
    const auto dispatch_pos = ci.find("\"indie_review_dispatch\"", block_start);
    ASSERT_NE(dispatch_pos, std::string::npos);
    const auto block_end = ci.find("// ANTS-1289", dispatch_pos);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);
    int count = 0;
    size_t pos = 0;
    while ((pos = region.find("additionalProperties", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    // ANTS-2068 — floor, not exact: every indie_review_* tool schema must
    // pin additionalProperties:false, so adding a tool shouldn't false-fail
    // this; a drop below the known floor means one was loosened/removed.
    EXPECT_GE(count, 3)
        << "expected >= 3 additionalProperties=false in the indie_review "
           "tool block (partition, corroborate, dispatch); "
           "fewer means a schema dropped its additionalProperties guard";
}
