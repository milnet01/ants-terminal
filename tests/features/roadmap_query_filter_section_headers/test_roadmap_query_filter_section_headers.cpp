// ANTS-1398 — feature-conformance test for the rollup-bullet
// filter on roadmap_query. Source-scrape style matching the
// sibling mcp_roadmap_status_filter test (ANTS-1247).

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

// INV-1 — req carries the include_section_headers flag.
TEST(roadmap_query_filter_section_headers, Inv1FlagReadFromReq) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "include_section_headers"),
           "INV-1: cmdRoadmapQuery reads include_section_headers "
           "from req");
    expect(contains(cpp, "ANTS-1398-INV-1"),
           "INV-1 anchor comment present in remotecontrol.cpp");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — rollup predicate tests id+headline emptiness.
TEST(roadmap_query_filter_section_headers, Inv2RollupPredicate) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1398-INV-2"),
           "INV-2 anchor comment present");
    // The predicate's job: drop bullets where both id and
    // headline are empty. Grep for the conjunction structure.
    expect(contains(cpp, "isRollupBullet"),
           "INV-2: isRollupBullet predicate present");
    EXPECT_EQ(0, expect_failures());
}

// INV-3a — full-file emission path filters rollups.
TEST(roadmap_query_filter_section_headers, Inv3aFullFilePathFilters) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1398-INV-3a"),
           "INV-3a anchor comment present (full-file emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3b — section-mode emission path filters rollups.
TEST(roadmap_query_filter_section_headers, Inv3bSectionPathFilters) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1398-INV-3b"),
           "INV-3b anchor comment present (section-mode emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — schema property advertised on the roadmap_query tool.
TEST(roadmap_query_filter_section_headers, Inv4SchemaPropertyAdded) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // The roadmap_query descriptor must list the new opt-in flag.
    //
    // ANTS-3720 — this was a fixed-byte window from the tool name, widened
    // six times (6→7→8→10→12→13→14→16 KiB) as SIBLING properties grew;
    // the ANTS-3698 `filter` alias would have made it seven. Every one of
    // those edits asserted nothing new, and the failure they fixed looks
    // exactly like the property having been deleted. The descriptor block
    // now bounds itself.
    const std::string region = ants_test::mcpToolDescriptor(ci, "roadmap_query");
    ASSERT_FALSE(region.empty())
        << "INV-4 precondition: roadmap_query tool name missing "
           "from claudeintegration.cpp";
    expect(contains(region, "include_section_headers"),
           "INV-4: include_section_headers schema property must "
           "be declared on the roadmap_query tool descriptor");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — opt-in echo emitted only when arg was set.
TEST(roadmap_query_filter_section_headers, Inv5EchoOnlyWhenSet) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1398-INV-5"),
           "INV-5 anchor comment present (echo emitted "
           "conditionally to keep wire trim)");
    EXPECT_EQ(0, expect_failures());
}
