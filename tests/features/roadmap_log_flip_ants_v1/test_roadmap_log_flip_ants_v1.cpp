// ANTS-1441 — feature-conformance test for roadmap_log op:"flip"
// ants-v1 native format support. Source-scrape style; matches the
// sibling ANTS-1428 GFM-flip test pattern.

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

// INV-1 — walker recognises ants-v1 shape + fence handling.
TEST(roadmap_log_flip_ants_v1, Inv1WalkerAnchors) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "walkAntsV1Bullets"),
           "INV-1: walkAntsV1Bullets helper defined");
    expect(contains(cpp, "rxAntsV1IdBracket"),
           "INV-1: bracket-ID regex compiled");
    // ANTS-2051 — the leading letter is now case-insensitive so the write
    // parser recognises lowercase project prefixes (`[mame-curator-1065]`)
    // exactly like the read path's idTokenPattern(). The old `[A-Z]…` form
    // rejected them, leaving markerless ants-v1 roadmaps read-only to
    // roadmap_log.
    expect(contains(cpp, "[A-Za-z][A-Za-z0-9_-]*-\\\\d{1,8}"),
           "INV-1: regex matches PREFIX-NNNN id shape, case-insensitive lead "
           "(ANTS-2051 — mirrors the read path's idTokenPattern)");
    expect(!contains(cpp, "[A-Z][A-Z0-9_-]*-\\\\d{1,8}"),
           "INV-1: the old uppercase-only lead is gone (regression guard for "
           "the lowercase-id refusal)");
    expect(contains(cpp, "insideFence"),
           "INV-1: walker tracks fenced-code state");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — GFM first, ants-v1 fallback, unified refusal.
TEST(roadmap_log_flip_ants_v1, Inv2FormatFallthroughOrder) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1441 — try ants-v1 native format"),
           "INV-2: fallthrough anchor present in cmdRoadmapLogFlip");
    expect(contains(cpp, "walkAntsV1Bullets(lines)"),
           "INV-2: ants-v1 walker called inside the zero-GFM branch");
    expect(contains(cpp,
        "neither GFM-task-list nor ants-v1 native format"),
        "INV-2: unified refusal message names both formats");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — anchor locator refused on ants-v1.
TEST(roadmap_log_flip_ants_v1, Inv3AnchorRejected) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // String spans a line break in source (`"is not " "supported..."`);
    // match the leading clause + the format-name suffix separately.
    expect(contains(cpp, "anchor locator is not"),
           "INV-3: anchor-locator clause present");
    expect(contains(cpp, "ants-v1 native format"),
           "INV-3: refusal names ants-v1 native format");
    expect(contains(cpp, "bad_op_combo"),
           "INV-3: refusal uses bad_op_combo code");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 / INV-5 — locator predicates.
TEST(roadmap_log_flip_ants_v1, Inv4Inv5LocatorPredicates) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "v1bullets.at(i).id == locId"),
           "INV-4: id locator matches bracket-id directly");
    expect(contains(cpp, "rcFnv1a64(rcNormaliseHeadline(locHeadline))"),
           "INV-5: headline locator pre-hashes the needle via shared rcFnv1a64+rcNormaliseHeadline");
    expect(contains(cpp, "rcNormaliseHeadline(v1bullets.at(i).headline)"),
           "INV-5: headline locator hashes each bullet headline through the same helpers");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — no anchor injection, no counter use; pure emoji swap.
TEST(roadmap_log_flip_ants_v1, Inv6PureEmojiSwap) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "applyAntsV1Flip"),
           "INV-6: applier helper defined");
    expect(contains(cpp, "line.remove(2, oldEmoji.size())"),
           "INV-6: applier removes old emoji at position 2");
    expect(contains(cpp, "line.insert(2, targetEmoji)"),
           "INV-6: applier inserts target emoji at position 2");
    // The applier MUST NOT contain anchor-injection or counter
    // bumping logic — they're GFM-only concerns. Check the helper
    // doesn't reference them.
    const auto applierIdx = cpp.find("void applyAntsV1Flip");
    ASSERT_NE(applierIdx, std::string::npos);
    const auto applierEnd = cpp.find("\n}\n", applierIdx);
    const std::string body = cpp.substr(applierIdx,
                                        applierEnd - applierIdx);
    expect(!contains(body, "anchorToInject"),
           "INV-6: applier body has no anchor injection");
    expect(!contains(body, ".roadmap-counter"),
           "INV-6: applier body has no counter touch");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — fenced bullets refused.
TEST(roadmap_log_flip_ants_v1, Inv7FencedRefusal) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // The refusal sits inside the ants-v1 branch.
    expect(contains(cpp, "v1target.insideFenced"),
           "INV-7: ants-v1 fenced-code check present");
    expect(contains(cpp, "located bullet is"),
           "INV-7: refusal names located bullet");
    expect(contains(cpp, "inside a fenced code block"),
           "INV-7: refusal names fenced code block context");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — success envelope carries format:"ants-v1".
TEST(roadmap_log_flip_ants_v1, Inv8SuccessEnvelopeFormat) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "out[\"format\"]          = QStringLiteral(\"ants-v1\")"),
           "INV-8: success envelope echoes format:\"ants-v1\"");
    expect(contains(cpp, "out[\"anchor_injected\"] = false"),
           "INV-8: anchor_injected:false (never injects on ants-v1)");
    expect(contains(cpp, "out[\"id\"]              = v1target.id"),
           "INV-8: id field carries the bracket-id");
    EXPECT_EQ(0, expect_failures());
}

// INV-9 (ANTS-2089) — return:"headline_only" echoes a post_bullets
// compact bullet on the flip success path, with the status emoji
// reversed to its word form.
TEST(roadmap_log_flip_ants_v1, Inv9HeadlineOnlyEcho) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rcStatusWord"),
           "INV-9: emoji->word helper rcStatusWord defined");
    expect(contains(cpp, "rcReturnHeadlineOnly(req)") &&
               contains(cpp, "out[\"post_bullets\"]") &&
               contains(cpp, "v1target.headline"),
           "INV-9: ants-v1 flip echoes post_bullets from v1target.headline "
           "under return:headline_only");
    EXPECT_EQ(0, expect_failures());
}
