// Feature-conformance test for spec.md (ANTS-1828 / ANTS-1829).
//
// Source-grep verification (mirrors image_bomb_png_header_peek): a live
// image-budget test would pass silently or hang CI, so we guard the
// regression class "a future edit drops the guard".

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_TERMINALGRID_CPP_PATH
#error "SRC_TERMINALGRID_CPP_PATH must be baked at compile time"
#endif
#ifndef SRC_INLINEIMAGEDECODE_PATH
#error "SRC_INLINEIMAGEDECODE_PATH must be baked at compile time"
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

// ANTS-1061 — the strict decoder both paths call, and the parse worker's half.
// Checked here so the strictness INV-2 and INV-3 rest on is checked once.
namespace {
std::string helperSrc() {
    return ants_test::slurpFile(SRC_INLINEIMAGEDECODE_PATH);
}
}  // namespace

TEST(TerminalGridImageHardening, SharedBase64DecoderIsStrict) {
    const std::string src = helperSrc();
    ASSERT_FALSE(src.empty());
    const std::string body = functionBody(src, "QByteArray decodeBase64Strict");
    ASSERT_FALSE(body.empty()) << "decodeBase64Strict not found";
    EXPECT_TRUE(contains(body, "AbortOnBase64DecodingErrors"))
        << "decodeBase64Strict must reject corrupt input";
    EXPECT_TRUE(contains(body, "fromBase64Encoding"))
        << "decodeBase64Strict must use the strict fromBase64Encoding decoder";
}

// INV-2 (ANTS-1829) — handleOscImage strict-decodes + caps the base64, and so
// does the parse worker's decode of the same payload (ANTS-1061).
TEST(TerminalGridImageHardening, ITerm2ImageStrictDecodeAndCap) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALGRID_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string body = functionBody(src, "TerminalGrid::handleOscImage");
    ASSERT_FALSE(body.empty()) << "handleOscImage not found";
    EXPECT_TRUE(contains(body, "decodeBase64Strict"))
        << "handleOscImage must strict-decode the base64 (reject corrupt input)";
    EXPECT_TRUE(contains(body, "kMaxInlineImageB64Bytes"))
        << "handleOscImage must bound the base64 size before decoding";

    const std::string worker = functionBody(helperSrc(), "Predecoder::onAction");
    ASSERT_FALSE(worker.empty()) << "Predecoder::onAction not found";
    EXPECT_TRUE(contains(worker, "decodeBase64Strict"))
        << "the worker's iTerm2 decode must be strict too";
    EXPECT_TRUE(contains(worker, "kMaxInlineImageB64Bytes"))
        << "the worker's iTerm2 decode must bound the base64 size first";
}

// INV-3 (ANTS-2119 M2) — the Kitty graphics path (handleApc) strict-decodes its
// base64 too, matching the OSC 52 / OSC 1337 image paths. It previously used the
// non-strict QByteArray::fromBase64, which silently skips invalid bytes and
// feeds a garbage-prefixed byte stream to the image loader / raw-pixel .copy().
TEST(TerminalGridImageHardening, KittyApcImageStrictDecode) {
    const std::string src = ants_test::slurpFile(SRC_TERMINALGRID_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const std::string body = functionBody(src, "TerminalGrid::handleApc");
    ASSERT_FALSE(body.empty()) << "handleApc not found";
    EXPECT_TRUE(contains(body, "decodeBase64Strict"))
        << "the Kitty APC image decode must strict-decode the base64";
    EXPECT_FALSE(contains(body, "fromBase64(base64Data)"))
        << "the non-strict fromBase64(base64Data) decode must be gone (ANTS-2119 M2)";

    const std::string worker = functionBody(helperSrc(), "Predecoder::onApc");
    ASSERT_FALSE(worker.empty()) << "Predecoder::onApc not found";
    EXPECT_TRUE(contains(worker, "decodeBase64Strict"))
        << "the worker's Kitty decode must be strict too";
}
