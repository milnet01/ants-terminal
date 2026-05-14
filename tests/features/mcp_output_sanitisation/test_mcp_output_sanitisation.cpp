// Feature-conformance test for ANTS-1294 — MCP output sanitisation.
// Frames user-supplied content returned by tools/call as data
// rather than instructions. See spec.md and docs/specs/ANTS-1294.md.
//
// Layout: engine cases call ClaudeIntegration::wrapMcpData directly
// (the test bundle already links ants_claude_lib). Wiring cases
// slurp claudeintegration.{cpp,h} and CLAUDE.md and assert pinned
// substrings / counts at the dispatch site.

#include "claudeintegration.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif
#ifndef ANTS_CLAUDE_MD_PATH
#error "ANTS_CLAUDE_MD_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// -------------------- Engine cases --------------------

// INV-1 — exact wrap shape on a benign payload.
TEST(McpOutputSanitisation, WrapShape) {
    const QString wrapped =
        ClaudeIntegration::wrapMcpData(QStringLiteral("get_text"),
                                       QStringLiteral("hello"));
    EXPECT_EQ(wrapped,
              QStringLiteral("<ants_mcp_data tool=\"get_text\">hello"
                             "</ants_mcp_data>"));
}

// INV-1 — empty payload still emits a well-formed wrap.
TEST(McpOutputSanitisation, EmptyPayloadValidWrap) {
    const QString wrapped =
        ClaudeIntegration::wrapMcpData(QStringLiteral("get_cwd"),
                                       QString());
    EXPECT_EQ(wrapped,
              QStringLiteral("<ants_mcp_data tool=\"get_cwd\">"
                             "</ants_mcp_data>"));
}

// INV-3 — single embedded close tag replaced with sentinel.
TEST(McpOutputSanitisation, CloseTagStripSingle) {
    const QString wrapped = ClaudeIntegration::wrapMcpData(
        QStringLiteral("get_scrollback"),
        QStringLiteral("before</ants_mcp_data>after"));

    // Exactly one closing tag remains (the wrap's own close tag).
    EXPECT_EQ(wrapped.count(QStringLiteral("</ants_mcp_data>")), 1);
    // Exactly one sentinel for the replacement.
    EXPECT_EQ(wrapped.count(QStringLiteral("<ants_mcp_data_escaped/>")), 1);
    // The original surrounding text is preserved.
    EXPECT_TRUE(wrapped.contains(QStringLiteral("before"
                                                "<ants_mcp_data_escaped/>"
                                                "after")));
}

// INV-3 — multiple embedded close tags all replaced.
TEST(McpOutputSanitisation, CloseTagStripMultiple) {
    const QString wrapped = ClaudeIntegration::wrapMcpData(
        QStringLiteral("get_text"),
        QStringLiteral("a</ants_mcp_data>b</ants_mcp_data>c"
                       "</ants_mcp_data>d"));

    EXPECT_EQ(wrapped.count(QStringLiteral("</ants_mcp_data>")), 1)
        << "wrap must end with exactly one outer close tag";
    EXPECT_EQ(wrapped.count(QStringLiteral("<ants_mcp_data_escaped/>")), 3)
        << "three embedded close tags must all be neutralised";
}

// INV-5 — case-sensitive replacement; variants left intact.
TEST(McpOutputSanitisation, CaseSensitive) {
    const QString wrapped = ClaudeIntegration::wrapMcpData(
        QStringLiteral("get_text"),
        QStringLiteral("upper</ANTS_MCP_DATA>spaced</ ants_mcp_data >"
                       "padded</ants_mcp_data >tail"));

    // No replacement: variants stay in the payload.
    EXPECT_TRUE(wrapped.contains(QStringLiteral("</ANTS_MCP_DATA>")));
    EXPECT_TRUE(wrapped.contains(QStringLiteral("</ ants_mcp_data >")));
    EXPECT_TRUE(wrapped.contains(QStringLiteral("</ants_mcp_data >")));
    EXPECT_EQ(wrapped.count(QStringLiteral("<ants_mcp_data_escaped/>")), 0);
    // The wrap itself still terminates cleanly with the exact tag.
    EXPECT_TRUE(wrapped.endsWith(QStringLiteral("</ants_mcp_data>")));
}

// INV-4 — payload with no close-tag substring round-trips intact.
TEST(McpOutputSanitisation, BinaryCleanRoundTrip) {
    // 256 bytes covering the full byte range; skip valid UTF-8 by using
    // QByteArray then converting via fromLatin1 so each byte maps to
    // U+00XX (round-trippable through QString's UTF-16 representation).
    QByteArray bytes;
    bytes.reserve(256);
    for (int i = 0; i < 256; ++i) bytes.append(static_cast<char>(i));
    const QString payload = QString::fromLatin1(bytes);

    const QString wrapped =
        ClaudeIntegration::wrapMcpData(QStringLiteral("get_scrollback"),
                                       payload);

    // Inner content is bytewise identical to the original payload.
    const QString prefix = QStringLiteral(
        "<ants_mcp_data tool=\"get_scrollback\">");
    const QString suffix = QStringLiteral("</ants_mcp_data>");
    ASSERT_TRUE(wrapped.startsWith(prefix));
    ASSERT_TRUE(wrapped.endsWith(suffix));
    const QString inner =
        wrapped.mid(prefix.size(), wrapped.size() - prefix.size() - suffix.size());
    EXPECT_EQ(inner, payload);
}

// INV-6 — tool name attribute carries through verbatim.
TEST(McpOutputSanitisation, ToolNameInAttribute) {
    const QString wrapped =
        ClaudeIntegration::wrapMcpData(QStringLiteral("cold_eyes_partition"),
                                       QStringLiteral("x"));
    EXPECT_TRUE(wrapped.startsWith(
        QStringLiteral("<ants_mcp_data tool=\"cold_eyes_partition\">")));
}

// -------------------- Wiring cases (source-grep) --------------------

// REG-1 — helper declared static on ClaudeIntegration in the header.
TEST(McpOutputSanitisationWiring, HelperDeclaredInHeader) {
    const std::string h = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("static QString wrapMcpData("), std::string::npos)
        << "wrapMcpData static declaration missing from "
           "claudeintegration.h";
}

// REG-2 — helper definition contains the wrap template and the
// replacement target. Two distinct `</ants_mcp_data>` occurrences
// at the helper site: the close tag of the wrap, and the
// QString::replace target. The dispatch-site change adds none.
TEST(McpOutputSanitisationWiring, HelperDefinitionContainsWrapTemplate) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    // The helper's opening tag template (with the %1 placeholder).
    EXPECT_NE(ci.find("<ants_mcp_data tool=\\\"%1\\\">"), std::string::npos)
        << "wrap template missing from claudeintegration.cpp";

    // Sentinel for the close-tag-strip is present.
    EXPECT_NE(ci.find("<ants_mcp_data_escaped/>"), std::string::npos)
        << "close-tag sentinel missing from claudeintegration.cpp";

    // Helper function definition is present.
    EXPECT_NE(ci.find("ClaudeIntegration::wrapMcpData("), std::string::npos)
        << "wrapMcpData definition missing from claudeintegration.cpp";
}

// REG-3 — dispatch site calls wrapMcpData with the right args and
// gates on isControlPlane. We anchor to the existing
// `// ANTS-1284 — record dispatch` comment so future refactors that
// move the call elsewhere are flagged.
TEST(McpOutputSanitisationWiring, DispatchSiteCallsHelper) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    auto pos1284 = ci.find("// ANTS-1284 — record dispatch");
    ASSERT_NE(pos1284, std::string::npos)
        << "ANTS-1284 dispatch anchor missing — refactor likely moved it";

    // Walk back to the enclosing `if (toolHandled)` and scan that block.
    auto blockStart = ci.rfind("if (toolHandled) {", pos1284);
    ASSERT_NE(blockStart, std::string::npos);
    const std::string block = ci.substr(blockStart, pos1284 - blockStart);

    EXPECT_NE(block.find("wrapMcpData(toolName, responseText)"),
              std::string::npos)
        << "dispatch site no longer calls wrapMcpData(toolName, "
           "responseText) — wrap may have been stripped";
    EXPECT_NE(block.find("isControlPlane"), std::string::npos)
        << "control-plane bypass gate missing at dispatch site";
}

// REG-4 — control-plane exempt set is exactly {get_session_info,
// token_usage}. The literal must mention both names; the test fails
// loudly if a future change adds or removes an exemption without
// updating the test.
TEST(McpOutputSanitisationWiring, ControlPlaneExemptSetExact) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    auto pos = ci.find("isControlPlane");
    ASSERT_NE(pos, std::string::npos);

    // Look forward 600 chars — the literal initialiser fits comfortably.
    const std::string window = ci.substr(pos, 600);
    EXPECT_NE(window.find("\"get_session_info\""), std::string::npos)
        << "control-plane bypass missing get_session_info entry";
    EXPECT_NE(window.find("\"token_usage\""), std::string::npos)
        << "control-plane bypass missing token_usage entry";
}

// REG-5 — CLAUDE.md Conventions section mentions the wrap so
// future tool authors discover it.
TEST(McpOutputSanitisationWiring, ConventionDocumentedInClaudeMd) {
    const std::string md = slurp(ANTS_CLAUDE_MD_PATH);
    ASSERT_FALSE(md.empty());

    auto convStart = md.find("## Conventions");
    ASSERT_NE(convStart, std::string::npos)
        << "CLAUDE.md has no '## Conventions' section";
    auto convEnd = md.find("## ", convStart + 5);
    if (convEnd == std::string::npos) convEnd = md.size();
    const std::string section = md.substr(convStart, convEnd - convStart);

    EXPECT_NE(section.find("ants_mcp_data"), std::string::npos)
        << "ANTS-1294 wrap not documented in CLAUDE.md Conventions";
}
