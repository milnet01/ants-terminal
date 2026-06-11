// Feature-conformance test for spec.md (ANTS-1828 / ANTS-1829).
//
// Source-grep verification (mirrors image_bomb_png_header_peek): a live
// image-budget test would pass silently or hang CI, so we guard the
// regression class "a future edit drops the guard".

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALGRID_CPP_PATH
#error "SRC_TERMINALGRID_CPP_PATH must be baked at compile time"
#endif

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the body of a function by brace-matching from its signature.
// ANTS-1468 — delegate to the shared string/comment-aware extractor.
std::string functionBody(const std::string &src, const char *signature) {
    return ants_test::slurpFunctionBody(src, signature);
}

}  // namespace

// INV-1 (ANTS-1828) — recomputeImageBudget counts the alt-screen images.
TEST(TerminalGridImageHardening, BudgetIncludesAltImages) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALGRID_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string body =
        functionBody(src, "TerminalGrid::recomputeImageBudget");
    ASSERT_FALSE(body.empty()) << "recomputeImageBudget not found";
    EXPECT_TRUE(contains(body, "m_altInlineImages"))
        << "recomputeImageBudget must sum m_altInlineImages so the per-terminal "
           "cap holds across a 1049 alt-screen swap";
    EXPECT_TRUE(contains(body, "m_inlineImages"));
    EXPECT_TRUE(contains(body, "m_kittyImages"));
}

// INV-2 (ANTS-1829) — handleOscImage strict-decodes + caps the base64.
TEST(TerminalGridImageHardening, ITerm2ImageStrictDecodeAndCap) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALGRID_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string body = functionBody(src, "TerminalGrid::handleOscImage");
    ASSERT_FALSE(body.empty()) << "handleOscImage not found";
    EXPECT_TRUE(contains(body, "AbortOnBase64DecodingErrors"))
        << "handleOscImage must strict-decode the base64 (reject corrupt input)";
    EXPECT_TRUE(contains(body, "fromBase64Encoding"))
        << "handleOscImage must use the strict fromBase64Encoding decoder";
    EXPECT_TRUE(contains(body, "kMaxInlineImageB64Bytes"))
        << "handleOscImage must bound the base64 size before decoding";
}
