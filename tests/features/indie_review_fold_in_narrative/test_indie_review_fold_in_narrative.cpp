// Feature-conformance test for spec.md (ANTS-1644 — indie-review side).
//
// Verifies the narrative-mode port from ANTS-1635 into
// cmdIndieReviewFoldIn:
//   - handler short-circuits on narrative_mode after the RcGate
//   - empty narrative_md refuses with `narrative_md_required`
//   - the actionable[] refusal names the escape hatch
//   - descriptor declares narrative_mode + narrative_md, drops
//     actionable from required, keeps caller_cwd required
//
// Pure source-grep — no engine link required. Mirrors the
// cold_eyes_fold_in_freeform test pattern (ANTS-1510).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Body window for cmdIndieReviewFoldIn — function start through the
// `cmdIndieReviewDispatch` neighbour. Plenty of slack for the actual
// body (~150 LOC) without overshooting into unrelated handlers.
std::string foldInBody(const std::string &rc) {
    const auto fnStart = rc.find("RemoteControl::cmdIndieReviewFoldIn");
    if (fnStart == std::string::npos) return {};
    const auto fnEnd = rc.find("RemoteControl::cmdIndieReviewDispatch",
                               fnStart);
    if (fnEnd == std::string::npos) {
        return rc.substr(fnStart);
    }
    return rc.substr(fnStart, fnEnd - fnStart);
}

// Block window for the descriptor — from the `t["name"] =
// "indie_review_fold_in"` line through the matching
// `tools.append(t);`.
std::string descriptorBlock(const std::string &ci) {
    const auto nameLine =
        ci.find("t[\"name\"] = \"indie_review_fold_in\"");
    if (nameLine == std::string::npos) return {};
    const auto blockEnd = ci.find("tools.append(t);", nameLine);
    if (blockEnd == std::string::npos) return {};
    return ci.substr(nameLine, blockEnd - nameLine);
}

}  // namespace

// INV-2 + INV-5 + INV-6 — handler shorts to narrative mode after the
// gate, before actionable validation, and does NOT allocate IDs.
TEST(IndieReviewFoldInNarrative, HandlerShortCircuitsOnNarrativeMode) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body = foldInBody(rc);
    ASSERT_FALSE(body.empty());

    EXPECT_TRUE(contains(body, "\"narrative_mode\""))
        << "handler must read narrative_mode arg";
    EXPECT_TRUE(contains(body, "\"narrative_md\""))
        << "handler must read narrative_md arg";

    // Heading literal in indie-review branch.
    EXPECT_TRUE(contains(body, "### 🔍 Indie-review fold-in ("))
        << "narrative-mode block must emit indie-review heading";

    // Narrative branch reuses insertBlock (atomic write inherited).
    EXPECT_TRUE(contains(body, "RoadmapFoldIn::insertBlock"))
        << "narrative branch must call insertBlock";

    // INV-5 — caller_cwd resolution runs BEFORE the narrative-mode read,
    // so narrative mode cannot bypass the cwd_bad refusal. ANTS-1630
    // replaced the RcGate focused-tab gate with caller-cwd anchoring via
    // resolveCallerCwdRoot; the ordering invariant is preserved against
    // the new resolution site.
    const auto gatePos = body.find("resolveCallerCwdRoot");
    const auto narrPos = body.find("\"narrative_mode\"");
    ASSERT_NE(gatePos, std::string::npos)
        << "resolveCallerCwdRoot must remain in the handler";
    ASSERT_NE(narrPos, std::string::npos);
    EXPECT_LT(gatePos, narrPos)
        << "narrative-mode branch must come AFTER the caller_cwd "
           "resolution (INV-5: resolution must not be bypassed)";

    // INV-6 — narrative branch is positioned BEFORE the
    // actionable[] validation.
    const auto actMissingPos = body.find("actionable array required");
    ASSERT_NE(actMissingPos, std::string::npos);
    EXPECT_LT(narrPos, actMissingPos)
        << "narrative_mode branch must appear BEFORE the "
           "actionable[] validation (escape-hatch positioning)";
}

// INV-3 — empty/whitespace narrative_md refuses with the dedicated
// `narrative_md_required` code.
TEST(IndieReviewFoldInNarrative, HandlerRefusesEmptyNarrativeMd) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body = foldInBody(rc);
    ASSERT_FALSE(body.empty());

    EXPECT_TRUE(contains(body, "narrative_md_required"))
        << "narrative-mode branch must use the dedicated "
           "narrative_md_required code on empty body";
}

// INV-4 — actionable[] refusal message names the narrative escape
// hatch so callers discover it without re-reading the descriptor.
TEST(IndieReviewFoldInNarrative, HandlerNamesEscapeHatchOnActionableMissing) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body = foldInBody(rc);
    ASSERT_FALSE(body.empty());

    const auto actPos = body.find("actionable array required");
    ASSERT_NE(actPos, std::string::npos);
    const std::string window = body.substr(actPos, 400);
    EXPECT_TRUE(contains(window, "narrative_mode"))
        << "actionable[] refusal must name narrative_mode";
    EXPECT_TRUE(contains(window, "narrative_md"))
        << "actionable[] refusal must name narrative_md";
}

// INV-9 — descriptor declares narrative_mode + narrative_md;
// actionable is NOT in required; caller_cwd IS.
TEST(IndieReviewFoldInNarrative, DescriptorSurfacesNarrativeProps) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const std::string block = descriptorBlock(ci);
    ASSERT_FALSE(block.empty());

    EXPECT_TRUE(contains(block, "props[\"narrative_mode\"]"))
        << "descriptor must declare narrative_mode prop";
    EXPECT_TRUE(contains(block, "props[\"narrative_md\"]"))
        << "descriptor must declare narrative_md prop";

    EXPECT_FALSE(contains(block, "req.append(\"actionable\")"))
        << "actionable must be dropped from the required array";
    EXPECT_TRUE(contains(block, "req.append(\"caller_cwd\")"))
        << "caller_cwd must remain required";
}
