// Feature-conformance test for tests/features/indie_review_truncation_flag/spec.md.
//
// Verifies ANTS-1344 — indie_review_corroborate / cross_doc_diff
// envelope surfaces `truncated`, `truncated_lanes`, `truncated_at_bytes`
// when the engine's 64 KiB scan cap clips an input.

#include <gtest/gtest.h>

#include "../../_support/srcgrep.h"

#include <string>

namespace {


// Brace-matched body of the named function so the envelope-shape grep
// doesn't false-positive on the wrong cmd. ANTS-2064 — was a fixed-size
// substr window; slurpFunctionBody tracks the real body as it grows.
// (The legacy `bytes` arg is ignored, kept so call sites need no edit.)
std::string fnWindow(const std::string &src, const std::string &needle,
                     size_t /*bytes*/ = 4096) {
    return ants_test::slurpFunctionBody(src, needle);
}

}  // namespace

// INV-1 — public constexpr in the engine header.
TEST(IndieReviewTruncationFlag, Inv1KMaxScanBytesExposed) {
    const std::string hdr = ants_test::slurpFile(SRC_INDIE_REVIEW_ENGINE_H_PATH);
    ASSERT_FALSE(hdr.empty()) << "indiereviewengine.h not readable";
    EXPECT_NE(hdr.find("constexpr int kMaxScanBytes = 64 * 1024"),
              std::string::npos)
        << "kMaxScanBytes constexpr missing from header (ANTS-1344 INV-1)";
}

// INV-2 — the corroboration envelope carries the truncation surface.
//
// ANTS-4814 split the pass into corroborateWithRoot so a test could drive it
// without a MainWindow; cmdIndieReviewCorroborate is now the thin m_main
// guard, and the envelope this invariant is about is built in the callee. The
// window is sized for a function that has since gained the near-miss and
// shared-symbol blocks — the truncation surface sits at its end, so a window
// too small reads as the surface being absent.
TEST(IndieReviewTruncationFlag, Inv2CorroborateEnvelope) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty()) << "remotecontrol.cpp not readable";
    const std::string body =
        fnWindow(rc, "RemoteControl::corroborateWithRoot", 24576);
    ASSERT_FALSE(body.empty())
        << "corroborateWithRoot definition not found";

    EXPECT_NE(body.find("truncatedLanes"), std::string::npos)
        << "truncatedLanes tracking absent from cmdIndieReviewCorroborate";
    EXPECT_NE(body.find("\"truncated\""), std::string::npos);
    EXPECT_NE(body.find("\"truncated_lanes\""), std::string::npos);
    EXPECT_NE(body.find("\"truncated_at_bytes\""), std::string::npos);
    EXPECT_NE(body.find("IndieReviewEngine::kMaxScanBytes"),
              std::string::npos);
}

// INV-3 — cmdCrossDocDiff parity.
TEST(IndieReviewTruncationFlag, Inv3CrossDocDiffEnvelope) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        fnWindow(rc, "RemoteControl::cmdCrossDocDiff", 8192);
    ASSERT_FALSE(body.empty())
        << "cmdCrossDocDiff definition not found";

    EXPECT_NE(body.find("truncatedLanes"), std::string::npos);
    EXPECT_NE(body.find("\"truncated\""), std::string::npos);
    EXPECT_NE(body.find("\"truncated_lanes\""), std::string::npos);
    EXPECT_NE(body.find("\"truncated_at_bytes\""), std::string::npos);
}

// INV-4 — envelope emission is gated on !truncatedLanes.isEmpty(), so
// the v1 happy-path shape is preserved.
TEST(IndieReviewTruncationFlag, Inv4GatedEmission) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    // Two distinct gates — one per command. Both must hold.
    size_t hits = 0;
    size_t pos = 0;
    while ((pos = rc.find("!truncatedLanes.isEmpty()", pos)) != std::string::npos) {
        ++hits;
        ++pos;
    }
    EXPECT_GE(hits, 2u)
        << "Expected ≥2 `!truncatedLanes.isEmpty()` gates (one each in "
           "cmdIndieReviewCorroborate + cmdCrossDocDiff); found "
        << hits;
}
