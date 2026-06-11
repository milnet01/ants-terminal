// Feature-conformance test for ANTS-1287 — `section` MCP wiring.
// See tests/features/mcp_roadmap_section_slice/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {


// Region: `// ANTS-1287` registration extension to the next
// `tools.append(roadmapTool)` line. Matches the source layout
// added to claudeintegration.cpp.
size_t roadmapToolBlockEnd(const std::string &ci, size_t start) {
    return ci.find("tools.append(roadmapTool);", start);
}

}  // namespace

// REG-1
TEST(McpRoadmapSectionSlice, SectionArgPresentInRegistration) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1287");
    ASSERT_NE(pos, std::string::npos)
        << "// ANTS-1287 anchor missing from claudeintegration.cpp";
    const auto end = roadmapToolBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);
    EXPECT_NE(region.find("\"section\""), std::string::npos)
        << "section arg missing from roadmap_query registration";
}

// REG-2
TEST(McpRoadmapSectionSlice, SchemaSectionPropertyOptional) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1287");
    ASSERT_NE(pos, std::string::npos);
    const auto end = roadmapToolBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);
    EXPECT_NE(region.find("sectionProp[\"type\"] = \"string\""),
              std::string::npos)
        << "section property type=\"string\" missing";
    EXPECT_NE(region.find("props[\"section\"] = sectionProp"),
              std::string::npos)
        << "section property not inserted into props";
    // Back-compat (INV-1): the roadmap_query registration must
    // NOT set `required`. (status was already optional; section
    // joins it.)
    EXPECT_EQ(region.find("schema[\"required\"]"), std::string::npos)
        << "roadmap_query schema unexpectedly sets a required array";
}

// REG-3
TEST(McpRoadmapSectionSlice, CmdRoadmapQueryExtractsSection) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"section\")).toString()"),
              std::string::npos)
        << "section arg not extracted in cmdRoadmapQuery";
    EXPECT_NE(rc.find("RoadmapIndex::buildIndex"), std::string::npos)
        << "buildIndex call missing from cmdRoadmapQuery";
    EXPECT_NE(rc.find("RoadmapIndex::findBySlug"), std::string::npos)
        << "findBySlug call missing from cmdRoadmapQuery";
    EXPECT_NE(rc.find("RoadmapIndex::sliceSection"), std::string::npos)
        << "sliceSection call missing from cmdRoadmapQuery";
}

// REG-4
TEST(McpRoadmapSectionSlice, BadSectionErrorCodePresent) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("\"bad_section\""), std::string::npos)
        << "bad_section error code not emitted on unknown slug";
}

// REG-5
TEST(McpRoadmapSectionSlice, SectionEchoHygieneMatchesInv11) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find("\"bad_section\"");
    ASSERT_NE(pos, std::string::npos);
    // Walk backwards ~1500 bytes to find the hygiene block tied to
    // bad_section. The 64-byte truncate and < 0x20 → '?' substitution
    // pattern must both appear in that window. Widened from 600 to
    // 1500 in ANTS-1524 to accommodate the new bad_case refusal
    // branch that legitimately sits between the hygiene block and
    // the bad_section emission.
    const size_t windowStart = pos > 1500 ? pos - 1500 : 0;
    const std::string region = rc.substr(windowStart, pos - windowStart);
    EXPECT_NE(region.find("verbatim.truncate(64)"), std::string::npos)
        << "section echo not capped at 64 bytes";
    EXPECT_NE(region.find("verbatim.at(i).unicode() < 0x20"),
              std::string::npos)
        << "section echo control-char substitution missing";
}

// REG-6
TEST(McpRoadmapSectionSlice, CacheMembersDeclaredInHeader) {
    const std::string rh = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rh.empty());
    EXPECT_NE(rh.find("m_roadmapIndex"), std::string::npos)
        << "m_roadmapIndex member missing from remotecontrol.h";
    EXPECT_NE(rh.find("m_roadmapSectionCache"), std::string::npos)
        << "m_roadmapSectionCache member missing from remotecontrol.h";
    EXPECT_NE(rh.find("QVector<RoadmapIndex::Section>"), std::string::npos)
        << "m_roadmapIndex type signature missing";
    EXPECT_NE(rh.find("QHash<QString, QJsonArray>"), std::string::npos)
        << "m_roadmapSectionCache type signature missing";
}

// REG-7
TEST(McpRoadmapSectionSlice, ProviderLambdaForwardsSection) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    // The lambda extracts `args.value("section")` with an isString()
    // gate (parity with the status arg) and forwards via req["section"].
    EXPECT_NE(mw.find("args.value(\"section\")"), std::string::npos)
        << "provider lambda does not extract section arg";
    EXPECT_NE(mw.find("req[\"section\"]"), std::string::npos)
        << "provider lambda does not forward section into req";
}
