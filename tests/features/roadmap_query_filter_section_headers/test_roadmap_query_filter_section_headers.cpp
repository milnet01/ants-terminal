// ANTS-1398 — feature-conformance test for the rollup-bullet
// filter on roadmap_query. Source-scrape style matching the
// sibling mcp_roadmap_status_filter test (ANTS-1247).

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — req carries the include_section_headers flag.
TEST(roadmap_query_filter_section_headers, Inv1FlagReadFromReq) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-3a"),
           "INV-3a anchor comment present (full-file emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3b — section-mode emission path filters rollups.
TEST(roadmap_query_filter_section_headers, Inv3bSectionPathFilters) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-3b"),
           "INV-3b anchor comment present (section-mode emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — schema property advertised on the roadmap_query tool.
TEST(roadmap_query_filter_section_headers, Inv4SchemaPropertyAdded) {
    expect_reset();
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // The roadmap_query descriptor must list the new opt-in
    // flag. Anchor on the roadmap_query name then grep nearby
    // for include_section_headers.
    const auto pos = ci.find("\"roadmap_query\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-4 precondition: roadmap_query tool name missing "
           "from claudeintegration.cpp";
    // 8 KiB window covers the description + properties block
    // without bleeding into the following tool descriptor.
    // ANTS-1622 bumped 6→7 KiB: section_index description grew
    // by ~800 B to enumerate the new `*_id_only` parallel counts
    // and the `legacy_format_sections[]` envelope field.
    // ANTS-1848 bumped 7→8 KiB: the description + `mode` property
    // grew to document `status` now shaping section_index emission.
    const std::string region = ci.substr(pos, 8000);
    expect(contains(region, "include_section_headers"),
           "INV-4: include_section_headers schema property must "
           "be declared on the roadmap_query tool descriptor");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — opt-in echo emitted only when arg was set.
TEST(roadmap_query_filter_section_headers, Inv5EchoOnlyWhenSet) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-5"),
           "INV-5 anchor comment present (echo emitted "
           "conditionally to keep wire trim)");
    EXPECT_EQ(0, expect_failures());
}
