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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
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
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-3a"),
           "INV-3a anchor comment present (full-file emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3b — section-mode emission path filters rollups.
TEST(roadmap_query_filter_section_headers, Inv3bSectionPathFilters) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-3b"),
           "INV-3b anchor comment present (section-mode emission "
           "path applies the filter)");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — schema property advertised on the roadmap_query tool.
TEST(roadmap_query_filter_section_headers, Inv4SchemaPropertyAdded) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
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
    // ANTS-1856 bumped 8→10 KiB: the `id` selector added a sentence
    // to the description + an `idProp` block, pushing
    // include_section_headers to offset ~8.5 KiB.
    // ANTS-1726 bumped 10→12 KiB: the plural `ids` selector added a
    // descriptor sentence + an `idsProp` block (~1.2 KiB), pushing
    // include_section_headers past offset 10 KiB.
    // ANTS-1696 bumped 12→13 KiB: the section_shape envelope hint
    // added a sentence to the descriptor (~400 B).
    // ANTS-2079 bumped 13→14 KiB: the description/detail split prepended
    // a ~570 B short `description` + comment ahead of the (now `detail`)
    // encyclopedic prose, pushing include_section_headers down by that much.
    // ANTS-3400/3402 bumped 14→16 KiB: the `status` enum gained the granular
    // lifecycle names + an expanded description, and a new `max_body_bytes`
    // prop + expanded include_body description added ~1.5 KiB ahead of
    // include_section_headers.
    const std::string region = ci.substr(pos, 16000);
    expect(contains(region, "include_section_headers"),
           "INV-4: include_section_headers schema property must "
           "be declared on the roadmap_query tool descriptor");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — opt-in echo emitted only when arg was set.
TEST(roadmap_query_filter_section_headers, Inv5EchoOnlyWhenSet) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1398-INV-5"),
           "INV-5 anchor comment present (echo emitted "
           "conditionally to keep wire trim)");
    EXPECT_EQ(0, expect_failures());
}
