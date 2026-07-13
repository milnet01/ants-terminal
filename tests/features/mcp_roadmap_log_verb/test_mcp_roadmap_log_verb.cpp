// ANTS-1424 — feature-conformance test for the roadmap_log MCP verb.
// Source-scrape pattern; behavioural fixture test deferred to v2.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — schema declared in tools/list with the required fields
// and enum constraints.
TEST(mcp_roadmap_log_verb, Inv1SchemaDeclared) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find("\"roadmap_log\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-1: roadmap_log name literal missing from "
           "claudeintegration.cpp";
    // 44 KiB window — the schema block keeps growing (ANTS-1428 added
    // op / to_status / id / anchor / prefix_hint; ANTS-1717/1793 added
    // the annotate op prose + the `note` property; ANTS-1690 added the
    // flip_batch op prose + the `locators` array block; ANTS-1878/1879
    // added create_section + append_batch ops with their bullets[] +
    // after_section / level / title / intro_body property surface;
    // ANTS-2078/2080 added per-bullet stable_id + the `return` prop;
    // ANTS-2126 added the `pass` property + pass-headings op prose, which
    // pushed `section` ~69 bytes past the old 32 KiB window — bumped to
    // 40 KiB; ANTS-3432 declared the bundle_row props
    // cells/header/position/sort_col (~2 KiB of prose), pushing the
    // `props["caller_cwd"]`/`props["section"]` assignments to ~40.7 KiB —
    // bumped to 44 KiB). Window sized to outrun the next couple of
    // additions; trim if it grows unnecessarily.
    const std::string region = ci.substr(pos, 44000);
    expect(contains(region, "\"caller_cwd\""),
           "INV-1: caller_cwd schema property present");
    expect(contains(region, "\"section\""),
           "INV-1: section schema property present");
    expect(contains(region, "\"status\""),
           "INV-1: status schema property present");
    expect(contains(region, "\"headline\""),
           "INV-1: headline schema property present");
    expect(contains(region, "\"kind\""),
           "INV-1: kind schema property present");
    expect(contains(region, "\"planned\"") &&
           contains(region, "\"shipped\""),
           "INV-1: status enum lists planned + shipped values");
    expect(contains(region, "\"implement\"") &&
           contains(region, "\"fix\""),
           "INV-1: kind enum lists implement + fix values");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — classified Required in callerCwdContractFor.
TEST(mcp_roadmap_log_verb, Inv2ContractIsRequired) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto helperPos = ci.find(
        "callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(helperPos, std::string::npos);
    const auto helperEnd = ci.find("\n}\n", helperPos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body =
        ci.substr(helperPos, helperEnd - helperPos);
    const auto pos = body.find("\"roadmap_log\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-2: roadmap_log missing from callerCwdContractFor "
           "classification table";
    const auto eol = body.find('\n', pos);
    ASSERT_NE(eol, std::string::npos);
    const std::string line = body.substr(pos, eol - pos);
    expect(line.find("C::Required") != std::string::npos,
           "INV-2: roadmap_log must be Required — mutates "
           "ROADMAP.md + .roadmap-counter");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — cmdRoadmapLog reads counter + writes back.
TEST(mcp_roadmap_log_verb, Inv3CounterAllocation) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1424-INV-3"),
           "INV-3 anchor comment present at counter-allocation site");
    expect(contains(cpp, ".roadmap-counter"),
           "INV-3: cmdRoadmapLog references .roadmap-counter "
           "filename");
    expect(contains(cpp, "id_hint"),
           "INV-3: cmdRoadmapLog handles id_hint arg");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — insertion point via RoadmapIndex.
TEST(mcp_roadmap_log_verb, Inv4SectionInsertionViaIndex) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1424-INV-4"),
           "INV-4 anchor comment present at section-insertion site");
    expect(contains(cpp, "RoadmapIndex::buildIndex") ||
           contains(cpp, "RoadmapIndex::findBySlug"),
           "INV-4: cmdRoadmapLog uses RoadmapIndex helpers");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — status → emoji map.
TEST(mcp_roadmap_log_verb, Inv5StatusEmojiMap) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1424-INV-5"),
           "INV-5 anchor comment present at status-emoji-map site");
    // All four status words must be in the cmdRoadmapLog body
    // (one branch per status). Use the QStringLiteral form since
    // that's what the implementation will use.
    expect(contains(cpp, "\"planned\""),
           "INV-5: planned status word present");
    expect(contains(cpp, "\"in-progress\""),
           "INV-5: in-progress status word present");
    expect(contains(cpp, "\"shipped\""),
           "INV-5: shipped status word present");
    expect(contains(cpp, "\"considered\""),
           "INV-5: considered status word present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — error paths return early.
TEST(mcp_roadmap_log_verb, Inv6ErrorEarlyReturn) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1424-INV-6"),
           "INV-6 anchor comment present");
    // Error codes the spec mandates.
    expect(contains(cpp, "\"missing_field\""),
           "INV-6: missing_field error code present");
    expect(contains(cpp, "\"bad_status\""),
           "INV-6: bad_status error code present");
    expect(contains(cpp, "\"bad_kind\""),
           "INV-6: bad_kind error code present");
    expect(contains(cpp, "\"bad_section\""),
           "INV-6: bad_section error code present");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — verb registered via registerToolProvider.
TEST(mcp_roadmap_log_verb, Inv7RegisteredInMainwindow) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "registerToolProvider(\"roadmap_log\""),
           "INV-7: roadmap_log not registered as MCP tool in "
           "mainwindow.cpp");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — success envelope carries the four response fields.
TEST(mcp_roadmap_log_verb, Inv8SuccessEnvelopeFields) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1424-INV-8"),
           "INV-8 anchor comment present at success-envelope site");
    expect(contains(cpp, "\"bytes_written\""),
           "INV-8: bytes_written field present in success envelope");
    EXPECT_EQ(0, expect_failures());
}
