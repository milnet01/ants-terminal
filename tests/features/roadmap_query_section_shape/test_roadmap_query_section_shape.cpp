// ANTS-1696 — section_shape hint for non-bullet sections. The shape
// helper (rcSectionShape) is a free function in remotecontrol.cpp's
// anonymous namespace, and the section-mode emit also lives in
// cmdRoadmapQuery — both unlinked by the feature bundle, so the
// contract is locked via source-grep anchors matching the sibling
// section / id-filter tests.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — table detection: `|`-leading content line → "table".
TEST(roadmap_query_section_shape, Inv1TableDetection) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rcSectionShape"),
           "INV-1: rcSectionShape helper present");
    expect(contains(cpp, "ANTS-1696"),
           "INV-1: ANTS-1696 anchor present in remotecontrol.cpp");
    expect(contains(cpp, "QLatin1String(\"table\")"),
           "INV-1: table shape literal emitted");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — prose fallback for content lines that don't start with `|`.
TEST(roadmap_query_section_shape, Inv2ProseFallback) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "QLatin1String(\"prose\")"),
           "INV-2: prose shape literal emitted");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — empty slice classifies as "empty" and the hint is suppressed.
TEST(roadmap_query_section_shape, Inv3EmptySuppressesHint) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "QLatin1String(\"empty\")"),
           "INV-3: empty shape literal emitted");
    // The emit-time check skips when shape == empty.
    expect(contains(cpp, "!= QLatin1String(\"empty\")"),
           "INV-3: empty-shape emit-time guard present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — hint conditional on parsed bullet set being empty
// (sectionBullets.isEmpty() AT PARSE TIME, not after status filter).
TEST(roadmap_query_section_shape, Inv4HintConditionalOnParseEmpty) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "sectionBullets.isEmpty()"),
           "INV-4: hint gated on parseBullets-empty result");
    expect(contains(cpp, "out[\"section_shape\"]"),
           "INV-4: section_shape envelope field emitted");
    expect(contains(cpp, "out[\"non_bullet_lines\"]"),
           "INV-4: non_bullet_lines envelope field emitted");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — shape cached by slug; cleared alongside the section cache.
TEST(roadmap_query_section_shape, Inv5ShapeCachedBySlug) {
    expect_reset();
    const std::string cppHeader = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    const std::string cpp       = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cppHeader, "m_roadmapSectionShape"),
           "INV-5: m_roadmapSectionShape member declared");
    expect(contains(cpp, "m_roadmapSectionShape.clear()"),
           "INV-5: shape cache cleared in lockstep with the bullets "
           "cache");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — schema description mentions section_shape hint.
TEST(roadmap_query_section_shape, Inv7SchemaMentionsHint) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "ANTS-1696"),
           "INV-7: ANTS-1696 anchor in roadmap_query descriptor");
    expect(contains(cpp, "section_shape"),
           "INV-7: descriptor mentions section_shape envelope field");
    EXPECT_EQ(0, expect_failures());
}
