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

constexpr const char *kToolNames[5] = {
    "indie_review_partition",
    "indie_review_brief",
    "indie_review_corroborate",
    "indie_review_synthesis_prompt",
    "indie_review_fold_in",
};

constexpr const char *kCmdMethods[5] = {
    "cmdIndieReviewPartition",
    "cmdIndieReviewBrief",
    "cmdIndieReviewCorroborate",
    "cmdIndieReviewSynthesisPrompt",
    "cmdIndieReviewFoldIn",
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

TEST(McpIndieReviewTools, AllSchemasUseAdditionalPropertiesFalse) {
    // Defensive: every new tool's inputSchema sets additionalProperties=false
    // so unknown keys are rejected. Verified by counting `additionalProperties`
    // occurrences inside the indie_review_* declarations region.
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto fold_in_pos = ci.find("indie_review_fold_in");
    ASSERT_NE(fold_in_pos, std::string::npos);
    // From the start of the indie_review_partition block to the end of
    // indie_review_fold_in, count `additionalProperties` occurrences.
    const auto block_start = ci.find("indie_review_partition");
    ASSERT_NE(block_start, std::string::npos);
    // End of the last block is the next `result["tools"] = tools;` after fold_in.
    const auto block_end = ci.find("result[\"tools\"] = tools;",
                                   fold_in_pos);
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);
    int count = 0;
    size_t pos = 0;
    while ((pos = region.find("additionalProperties", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 5) << "expected 5 additionalProperties=false (one per tool)";
}
