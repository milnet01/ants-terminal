// ANTS-1399 — feature-conformance test for the tool_info MCP verb.
// Source-scrape pattern.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// ANTS-1597 — read the ~200 KB claudeintegration.cpp once and share it
// across all TESTs instead of re-slurping it per case.
const std::string &ciSource() {
    static const std::string s =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    return s;
}

}  // namespace

// INV-1 — tool_info declared as a tool in tools/list with
// required name + additionalProperties:false.
TEST(mcp_tool_info_verb, Inv1DescriptorDeclared) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "\"tool_info\""),
           "INV-1: tool_info name literal present in "
           "claudeintegration.cpp");
    expect(contains(ci, "ANTS-1399-INV-1"),
           "INV-1 anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — tools/list end stores tools into m_lastToolsList.
TEST(mcp_tool_info_verb, Inv2SnapshotPopulatedOnToolsList) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "ANTS-1399-INV-2"),
           "INV-2 anchor comment present");
    expect(contains(ci, "m_lastToolsList = tools"),
           "INV-2: snapshot assignment present at tools/list end");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — handler scans m_lastToolsList and emits the
// per-tool descriptor slice.
TEST(mcp_tool_info_verb, Inv3HandlerEmitsDescriptorSlice) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "ANTS-1399-INV-3"),
           "INV-3 anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — unknown-name emits code:"unknown_tool" + available[].
TEST(mcp_tool_info_verb, Inv4UnknownToolEnvelope) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "\"unknown_tool\""),
           "INV-4: unknown_tool code literal present");
    expect(contains(ci, "\"available\""),
           "INV-4: available[] field emitted on the "
           "unknown-tool envelope");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — missing-name emits code:"missing_name".
TEST(mcp_tool_info_verb, Inv5MissingNameEnvelope) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "\"missing_name\""),
           "INV-5: missing_name code literal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — cold-snapshot emits code:"tools_not_ready".
TEST(mcp_tool_info_verb, Inv6ColdSnapshotEnvelope) {
    expect_reset();
    const std::string &ci = ciSource();
    expect(contains(ci, "\"tools_not_ready\""),
           "INV-6: tools_not_ready code literal present");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — classified ProcessGlobal in callerCwdContractFor.
TEST(mcp_tool_info_verb, Inv7ContractIsProcessGlobal) {
    expect_reset();
    const std::string &ci = ciSource();
    const auto helperPos = ci.find(
        "callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(helperPos, std::string::npos)
        << "INV-7 precondition: callerCwdContractFor helper "
           "missing from claudeintegration.cpp";
    const auto helperEnd = ci.find("\n}\n", helperPos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = ci.substr(helperPos,
                                       helperEnd - helperPos);
    const auto pos = body.find("\"tool_info\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-7: tool_info missing from callerCwdContractFor "
           "classification table";
    const auto eol = body.find('\n', pos);
    ASSERT_NE(eol, std::string::npos);
    const std::string line = body.substr(pos, eol - pos);
    expect(line.find("C::ProcessGlobal") != std::string::npos,
           "INV-7: tool_info must be ProcessGlobal — reads the "
           "process-wide tool registry, not per-tab state");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — handler dispatched inline alongside get_session_info,
// not via m_toolProviders.
TEST(mcp_tool_info_verb, Inv8DispatchedInline) {
    expect_reset();
    const std::string &ci = ciSource();
    // Locate the get_session_info dispatch branch. tool_info
    // must be in the same `if/else if` chain — i.e. dispatched
    // before the m_toolProviders lookup.
    const auto sessPos = ci.find(
        "toolName == \"get_session_info\"");
    ASSERT_NE(sessPos, std::string::npos)
        << "INV-8 precondition: get_session_info inline branch "
           "missing — claudeintegration.cpp may have regressed";
    // 4000-byte window covers a few following else-if branches.
    const std::string region = ci.substr(sessPos, 4000);
    expect(contains(region, "tool_info"),
           "INV-8: tool_info must be dispatched inline next to "
           "get_session_info (same toolHandled-conditional block)");
    EXPECT_EQ(0, expect_failures());
}

// ---- ANTS-1985 — catalog mode (INV-9..INV-13) -------------------
// These continue the invariant sequence already owned by this test
// home (ANTS-1399 INV-1..8); see docs/specs/ANTS-1985.md § 3.

// Scope a substring to the tool_info descriptor registration block:
// from `t["name"] = "tool_info";` to the next `tools.append(t);`.
static std::string toolInfoDescriptorBlock(const std::string &ci) {
    const auto start = ci.find("t[\"name\"] = \"tool_info\";");
    if (start == std::string::npos) return std::string();
    const auto end = ci.find("tools.append(t);", start);
    if (end == std::string::npos) return ci.substr(start);
    return ci.substr(start, end - start);
}

// Scope a substring to the inline tool_info handler region: from the
// `else if (toolName == "tool_info")` branch to the next provider
// dispatch (`else if (auto it = m_toolProviders.find`).
static std::string toolInfoHandlerRegion(const std::string &ci) {
    const auto pos = ci.find("else if (toolName == \"tool_info\")");
    if (pos == std::string::npos) return std::string();
    const auto end = ci.find("m_toolProviders.find(toolName)", pos);
    if (end == std::string::npos) return ci.substr(pos);
    return ci.substr(pos, end - pos);
}

// INV-9 — descriptor declares a boolean `catalog` property and no
// longer lists `name` in `required`.
TEST(mcp_tool_info_verb, Inv9CatalogPropertyDeclared) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string block = toolInfoDescriptorBlock(ci);
    ASSERT_FALSE(block.empty())
        << "INV-9 precondition: tool_info descriptor block missing";
    expect(contains(block, "props[\"catalog\"]"),
           "INV-9: tool_info descriptor declares a `catalog` property");
    expect(contains(block, "catalogProp[\"type\"] = \"boolean\""),
           "INV-9: `catalog` property is typed boolean");
    expect(contains(block, "schema[\"additionalProperties\"] = false"),
           "INV-9: additionalProperties stays false");
    expect(!contains(block, "req.append(QStringLiteral(\"name\"))"),
           "INV-9: `name` is no longer appended to the tool_info "
           "schema's required array");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — catalog branch returns the grouped envelope keys.
TEST(mcp_tool_info_verb, Inv10CatalogEnvelope) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string region = toolInfoHandlerRegion(ci);
    ASSERT_FALSE(region.empty())
        << "INV-10 precondition: tool_info handler region missing";
    expect(contains(region, "catalogMode"),
           "INV-10: handler reads a catalogMode flag");
    expect(contains(region, "env[\"catalog\"]"),
           "INV-10: catalog branch emits the `catalog` envelope key");
    expect(contains(region, "env[\"tool_count\"]"),
           "INV-10: catalog branch emits `tool_count`");
    expect(contains(region, "env[\"category_count\"]"),
           "INV-10: catalog branch emits `category_count`");
    EXPECT_EQ(0, expect_failures());
}

// INV-11 — catalog mode with an empty snapshot emits tools_not_ready.
TEST(mcp_tool_info_verb, Inv11CatalogColdSnapshot) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string region = toolInfoHandlerRegion(ci);
    ASSERT_FALSE(region.empty());
    // The catalog sub-block runs from `if (catalogMode)` to the
    // `} else if (reqName.isEmpty())` that starts the legacy chain.
    const auto cstart = region.find("if (catalogMode)");
    const auto cend = region.find("} else if (reqName.isEmpty())");
    ASSERT_NE(cstart, std::string::npos)
        << "INV-11: catalogMode branch missing";
    ASSERT_NE(cend, std::string::npos)
        << "INV-11: legacy missing_name branch missing after catalog";
    ASSERT_LT(cstart, cend)
        << "INV-11: catalog branch must precede the missing_name guard";
    const std::string catalogBlock = region.substr(cstart, cend - cstart);
    expect(contains(catalogBlock, "tools_not_ready"),
           "INV-11: empty snapshot in catalog mode emits "
           "tools_not_ready");
    EXPECT_EQ(0, expect_failures());
}

// INV-12 — category derived from the [<kind>] prefix with an `other`
// fallback; categories + names emitted in sorted order (QMap).
TEST(mcp_tool_info_verb, Inv12CategoryDerivationSorted) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string region = toolInfoHandlerRegion(ci);
    ASSERT_FALSE(region.empty());
    expect(contains(region, "categoryOf"),
           "INV-12: a categoryOf helper parses the [<kind>] prefix");
    expect(contains(region, "QStringLiteral(\"other\")"),
           "INV-12: malformed/absent prefix falls back to `other`");
    expect(contains(region, "QMap<QString, QMap<QString, QString>>"),
           "INV-12: grouping via sorted QMap (categories + names "
           "emit in ascending order)");
    EXPECT_EQ(0, expect_failures());
}

// INV-13 — legacy branches survive: missing_name + unknown_tool still
// present, and the catalog branch is gated on the explicit flag (not
// on empty name).
TEST(mcp_tool_info_verb, Inv13LegacyBranchesIntact) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string region = toolInfoHandlerRegion(ci);
    ASSERT_FALSE(region.empty());
    expect(contains(region, "missing_name"),
           "INV-13: missing_name branch preserved");
    expect(contains(region, "unknown_tool"),
           "INV-13: unknown_tool branch preserved");
    expect(contains(region, "argsObj.value(QStringLiteral(\"catalog\"))"),
           "INV-13: catalog mode gated on the explicit `catalog` arg, "
           "never on empty name");
    EXPECT_EQ(0, expect_failures());
}
