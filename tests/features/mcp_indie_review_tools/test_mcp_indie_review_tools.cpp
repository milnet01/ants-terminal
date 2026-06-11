// Feature-conformance test for ANTS-1112 MCP wiring. Source-grep
// approach: read claudeintegration.cpp + mainwindow.cpp + remotecontrol.h/.cpp
// and verify all 5 tool names are registered in each layer.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

constexpr const char *kToolNames[6] = {
    "indie_review_partition",
    "indie_review_brief",
    "indie_review_corroborate",
    "indie_review_synthesis_prompt",
    "indie_review_fold_in",
    "indie_review_orchestrate",  // ANTS-1279
};

constexpr const char *kCmdMethods[6] = {
    "cmdIndieReviewPartition",
    "cmdIndieReviewBrief",
    "cmdIndieReviewCorroborate",
    "cmdIndieReviewSynthesisPrompt",
    "cmdIndieReviewFoldIn",
    "cmdIndieReviewOrchestrate",  // ANTS-1279
};

}  // namespace

TEST(McpIndieReviewTools, Inv9AllToolNamesInToolsList) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *name : kToolNames) {
        const std::string nameQuoted = std::string("\"") + name + "\"";
        EXPECT_NE(ci.find(nameQuoted), std::string::npos)
            << "tool name " << name << " missing from claudeintegration.cpp";
    }
}

TEST(McpIndieReviewTools, AllProvidersRegisteredInMainWindow) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
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
    const std::string rch = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    for (const char *m : kCmdMethods) {
        EXPECT_NE(rch.find(m), std::string::npos)
            << "method " << m << " missing from remotecontrol.h";
    }
}

TEST(McpIndieReviewTools, AllCmdMethodsDefinedInCpp) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    for (const char *m : kCmdMethods) {
        const std::string defn = std::string("RemoteControl::") + m;
        EXPECT_NE(rc.find(defn), std::string::npos)
            << "definition RemoteControl::" << m
            << " missing from remotecontrol.cpp";
    }
}

// ANTS-1279 — the orchestrate handler composes the dispatch manifest from
// the existing engine functions and emits the report-collection contract.
TEST(McpIndieReviewTools, Ants1279OrchestrateComposesManifest) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto p = rc.find("RemoteControl::cmdIndieReviewOrchestrate");
    ASSERT_NE(p, std::string::npos);
    const std::string body = rc.substr(p, 3500);
    EXPECT_NE(body.find("derivePartition"), std::string::npos)
        << "orchestrate must derive the partition";
    EXPECT_NE(body.find("assembleBriefManifest"), std::string::npos)
        << "orchestrate must assemble per-lane brief manifests";
    EXPECT_NE(body.find("suggestedMerges"), std::string::npos)
        << "orchestrate must surface suggested_merges (ANTS-1288 reuse)";
    EXPECT_NE(body.find("reports_dir"), std::string::npos)
        << "orchestrate must emit a reports_dir for the collect phase";
    EXPECT_NE(body.find("report_path"), std::string::npos)
        << "orchestrate must emit a per-lane report_path";
    EXPECT_NE(body.find("next_steps"), std::string::npos)
        << "orchestrate must emit next_steps wiring to corroborate/fold_in";
    EXPECT_NE(body.find("include_briefs"), std::string::npos)
        << "orchestrate must honour the include_briefs toggle";
}

// ANTS-1288 — the partition handler emits suggested_merges, computed via
// the engine helper (locks the wiring against accidental removal).
TEST(McpIndieReviewTools, Ants1288PartitionEmitsSuggestedMerges) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto p = rc.find("RemoteControl::cmdIndieReviewPartition");
    ASSERT_NE(p, std::string::npos);
    const std::string body = rc.substr(p, 2000);
    EXPECT_NE(body.find("suggested_merges"), std::string::npos)
        << "cmdIndieReviewPartition no longer emits suggested_merges";
    EXPECT_NE(body.find("IndieReviewEngine::suggestedMerges"),
              std::string::npos)
        << "cmdIndieReviewPartition no longer calls the engine helper";
}

TEST(McpIndieReviewTools, AllSchemasUseAdditionalPropertiesFalse) {
    // Defensive: every new tool's inputSchema sets additionalProperties=false
    // so unknown keys are rejected. Region scoped to JUST the indie_review_*
    // block — start at indie_review_partition, end at the next non-indie
    // tool block (currently debt_sweep_scan, ANTS-1113). ANTS-1352 added
    // indie_review_dispatch within this region (sixth tool).
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("\"indie_review_partition\"");
    ASSERT_NE(block_start, std::string::npos);
    // End: the next ANTS-NNNN comment after fold_in marks the next
    // tool series. Falls back to `result["tools"] = tools;` when no
    // subsequent block exists.
    const auto fold_in_pos = ci.find("\"indie_review_fold_in\"", block_start);
    ASSERT_NE(fold_in_pos, std::string::npos);
    auto block_end = ci.find("// ANTS-1113", fold_in_pos);
    if (block_end == std::string::npos) {
        block_end = ci.find("result[\"tools\"] = tools;", fold_in_pos);
    }
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
    EXPECT_GE(count, 7)
        << "expected >= 7 additionalProperties=false in the indie_review "
           "tool block (5 original indie_review_* tools + ANTS-1352 "
           "indie_review_dispatch + ANTS-1279 indie_review_orchestrate); "
           "fewer means a schema dropped its additionalProperties guard";
}
