// Feature-conformance test for ANTS-1409 — callerCwdSuffix helper.
// See tests/features/mcp_caller_cwd_suffix_helper/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef ANTS_CLAUDEINTEGRATION_SOURCES
#error "ANTS_CLAUDEINTEGRATION_SOURCES compile definition required"
#endif

namespace {


// Return the substring spanning `name`'s tool descriptor block —
// from the line that sets `<name>Tool["name"]` (or equivalent
// QJsonObject naming) through the next `tools.append(` call.
// Used by the per-tool INV-3 / INV-4 checks. Returns "" on miss.
std::string descriptorBlock(const std::string &src,
                            const std::string &toolName) {
    // Three QJsonObject naming conventions appear in the file:
    //   scrollbackTool, cwdTool, lastCmdTool, gitTool, envTool,
    //   getTextTool, ...  All end with the "name" assignment that
    //   carries the literal tool-name string. Anchor on that
    //   literal — it's unique enough.
    const std::string needle = "\"name\"] = \"" + toolName + "\"";
    const auto pos = src.find(needle);
    if (pos == std::string::npos) return {};
    const auto end = src.find("tools.append(", pos);
    if (end == std::string::npos) return {};
    return src.substr(pos, end - pos);
}

}  // namespace

// INV-1 — callerCwdSuffix helper lives near makeCallerCwdReadProp.
TEST(McpCallerCwdSuffixHelper, HelperDeclaredNearReadProp) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const auto readPropPos = cc.find("makeCallerCwdReadProp");
    ASSERT_NE(readPropPos, std::string::npos)
        << "ANTS-1409 INV-1 precondition: makeCallerCwdReadProp "
           "helper missing — ANTS-1391 may have regressed";
    const auto suffixPos = cc.find("callerCwdSuffix", readPropPos);
    ASSERT_NE(suffixPos, std::string::npos)
        << "ANTS-1409 INV-1: callerCwdSuffix helper missing from "
           "claudeintegration.cpp";
    EXPECT_LT(suffixPos - readPropPos, static_cast<size_t>(5000))
        << "ANTS-1409 INV-1: callerCwdSuffix helper not adjacent "
           "to makeCallerCwdReadProp (must share the tools/list "
           "helpers in claudeintegrationdetail)";
}

// INV-2 — the helper returns the canonical literal byte-for-byte.
TEST(McpCallerCwdSuffixHelper, ReturnsCanonicalLiteral) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    // ANTS-3681 — the brace-matched function body, not a 600-byte window
    // from the first mention (which is a comment above the helper).
    const std::string region =
        ants_test::slurpFunctionBody(cc, "QString callerCwdSuffix()");
    ASSERT_FALSE(region.empty());
    EXPECT_NE(region.find(
        "Pass `caller_cwd` to anchor to your tab (ANTS-1392)."),
              std::string::npos)
        << "ANTS-1409 INV-2: callerCwdSuffix must return the "
           "exact canonical literal — copy edits to the "
           "phrasing must go through this one helper";
}

// INV-3a — get_last_command descriptor calls the helper.
TEST(McpCallerCwdSuffixHelper, LastCommandUsesHelper) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string block = descriptorBlock(cc, "get_last_command");
    ASSERT_FALSE(block.empty())
        << "ANTS-1409 INV-3a: get_last_command descriptor block "
           "not locatable";
    EXPECT_NE(block.find("callerCwdSuffix()"), std::string::npos)
        << "ANTS-1409 INV-3a: get_last_command must call "
           "callerCwdSuffix() instead of spelling out the suffix";
}


// INV-3c — get_environment descriptor calls the helper.
TEST(McpCallerCwdSuffixHelper, EnvironmentUsesHelper) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string block = descriptorBlock(cc, "get_environment");
    ASSERT_FALSE(block.empty())
        << "ANTS-1409 INV-3c: get_environment descriptor block "
           "not locatable";
    EXPECT_NE(block.find("callerCwdSuffix()"), std::string::npos)
        << "ANTS-1409 INV-3c: get_environment must call "
           "callerCwdSuffix() instead of spelling out the suffix";
}

// INV-4a — get_scrollback keeps its long-form phrasing.
TEST(McpCallerCwdSuffixHelper, ScrollbackKeepsLongForm) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string block = descriptorBlock(cc, "get_scrollback");
    ASSERT_FALSE(block.empty())
        << "ANTS-1409 INV-4a: get_scrollback descriptor block "
           "not locatable";
    EXPECT_EQ(block.find("callerCwdSuffix()"), std::string::npos)
        << "ANTS-1409 INV-4a: get_scrollback must keep its "
           "long-form caller_cwd phrasing — the canonical short "
           "suffix omits the focused-tab fallback caveat";
}

// INV-4b — get_text keeps its inline-arg-list form.
TEST(McpCallerCwdSuffixHelper, GetTextKeepsInlineForm) {
    const std::string cc = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(cc.empty());
    const std::string block = descriptorBlock(cc, "get_text");
    ASSERT_FALSE(block.empty())
        << "ANTS-1409 INV-4b: get_text descriptor block not "
           "locatable";
    EXPECT_EQ(block.find("callerCwdSuffix()"), std::string::npos)
        << "ANTS-1409 INV-4b: get_text must keep its inline-arg-"
           "list caller_cwd phrasing — the canonical short suffix "
           "is a trailing sentence, not an arg-list entry";
}
