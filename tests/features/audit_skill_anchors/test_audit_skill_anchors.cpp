// ANTS-1410 — spec-anchor drift detection. The shipped /audit
// skill lives at ~/.claude/skills/audit/SKILL.md (outside this
// repo). We don't source-scrape the global file; we scrape
// docs/specs/ANTS-1410.md for the anchors the skill MUST contain.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — spec file readable.
TEST(audit_skill_anchors, Inv1SpecPresent) {
    expect_reset();
    const std::string spec = ants_test::slurpFile(SPEC_ANTS_1410_PATH);
    expect(spec.size() > 1000,
           "INV-1: ANTS-1410.md is non-trivial size");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — step 1.5 anchor.
TEST(audit_skill_anchors, Inv2Step1_5Anchor) {
    expect_reset();
    const std::string spec = ants_test::slurpFile(SPEC_ANTS_1410_PATH);
    expect(contains(spec, "Enumerate project-local CI gates"),
           "INV-2: step 1.5 anchor present");
    expect(contains(spec, "step 1.5"),
           "INV-2: numbered-step reference present");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — all 7 spec INVs documented.
TEST(audit_skill_anchors, Inv3SpecInvariantsDocumented) {
    expect_reset();
    const std::string spec = ants_test::slurpFile(SPEC_ANTS_1410_PATH);
    for (int n = 1; n <= 7; ++n) {
        const std::string needle =
            std::string("**INV-") + std::to_string(n) + " /";
        expect(contains(spec, needle),
               (std::string("INV-3: spec INV-") + std::to_string(n) +
                " anchor present").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — exclusion lists named.
TEST(audit_skill_anchors, Inv4ExclusionListsPresent) {
    expect_reset();
    const std::string spec = ants_test::slurpFile(SPEC_ANTS_1410_PATH);
    // Binary-name exclusion table.
    expect(contains(spec, "ruff") && contains(spec, "bandit") &&
           contains(spec, "cppcheck") && contains(spec, "semgrep") &&
           contains(spec, "gitleaks"),
           "INV-4: binary-name exclusion list present");
    // Formatter exclusion list.
    expect(contains(spec, "black") && contains(spec, "prettier") &&
           contains(spec, "clang-format"),
           "INV-4: formatter exclusion list present");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — pair-spec ref to ANTS-1351.
TEST(audit_skill_anchors, Inv5PairsWithAnts1351) {
    expect_reset();
    const std::string spec = ants_test::slurpFile(SPEC_ANTS_1410_PATH);
    expect(contains(spec, "ANTS-1351"),
           "INV-5: cross-ref to ANTS-1351 present");
    expect(contains(spec, "stopgap"),
           "INV-5: spec identifies itself as stopgap until ANTS-1351");
    EXPECT_EQ(0, expect_failures());
}
