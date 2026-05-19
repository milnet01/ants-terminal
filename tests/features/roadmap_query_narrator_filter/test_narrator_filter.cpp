// ANTS-1425 — feature-conformance test for the narrator-bullet
// filter v2. Source-scrape style matching the sibling ANTS-1398 test.

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

// INV-1 — opt-in arg read from req with default false.
TEST(roadmap_query_narrator_filter, Inv1OptInArgReadFromReq) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "include_narrator_bullets"),
           "INV-1: cmdRoadmapQuery reads include_narrator_bullets "
           "from req");
    expect(contains(cpp, "hasIncludeNarratorsArg"),
           "INV-1: hasIncludeNarratorsArg flag tracks whether the "
           "caller explicitly set the opt-in");
    expect(contains(cpp, "ANTS-1425"),
           "INV-1: ANTS-1425 anchor present");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — narrator predicate is the empty-id + non-empty-headline
// complement of the rollup predicate.
TEST(roadmap_query_narrator_filter, Inv2NarratorPredicate) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "isNarratorBullet"),
           "INV-2: isNarratorBullet predicate present");
    // The predicate must check `id.isEmpty() && !headline.isEmpty()`
    // as the disjoint complement of isRollupBullet.
    expect(contains(cpp, "!o.value(QStringLiteral(\"headline\"))"),
           "INV-2: narrator predicate inverts the headline check vs "
           "rollup predicate");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — single drop helper composes both opt-ins.
TEST(roadmap_query_narrator_filter, Inv3SingleDropHelper) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "shouldDropUnnumbered"),
           "INV-3: shouldDropUnnumbered helper unifies the two "
           "predicates so the filter loops have one call site");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — schema advertises the new property.
TEST(roadmap_query_narrator_filter, Inv4SchemaAdvertises) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "include_narrator_bullets"),
           "INV-4: claudeintegration's tools/list descriptor "
           "advertises include_narrator_bullets");
    expect(contains(cpp, "ANTS-1425"),
           "INV-4: schema-block ANTS-1425 anchor present");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — dispatch forwards the arg.
TEST(roadmap_query_narrator_filter, Inv5DispatchForwards) {
    expect_reset();
    const std::string cpp = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(cpp, "include_narrator_bullets"),
           "INV-5: mainwindow's roadmap_query lambda forwards "
           "include_narrator_bullets from args into req");
    expect(contains(cpp, "ANTS-1425"),
           "INV-5: dispatch-side ANTS-1425 anchor present");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — echo only when caller set it.
TEST(roadmap_query_narrator_filter, Inv6EchoOnlyWhenSet) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Two echo sites — both must guard on hasIncludeNarratorsArg.
    expect(contains(cpp,
                    "if (hasIncludeNarratorsArg) {\n            "
                    "out[\"include_narrator_bullets\"]"),
           "INV-6: section-mode echo guarded by "
           "hasIncludeNarratorsArg");
    expect(contains(cpp,
                    "if (hasIncludeNarratorsArg) {\n        "
                    "out[\"include_narrator_bullets\"]"),
           "INV-6: full-file echo guarded by hasIncludeNarratorsArg");
    EXPECT_EQ(0, expect_failures());
}
