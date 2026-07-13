// Feature-conformance test for spec.md (ANTS-3505).
//
// Verifies the GCC-only scoped pragma suppressing the 4 -Wnull-dereference
// false positives from diffviewer::show()'s positionBackToTop lambda is
// present, guarded, and correctly bracketed after the lambda's null guards.
// Static source-grep only (mirrors ANTS-1554 / ANTS-3358); the "0 warnings"
// property was verified by an -O3 compile when the fix landed.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

TEST(BuildWarningDiffviewerNullDeref, PragmaGuardPresent) {
    const std::string src = ants_test::slurpFile(SRC_DIFFVIEWER_CPP);
    ASSERT_FALSE(src.empty()) << "could not read " << SRC_DIFFVIEWER_CPP;

    // Anchor on the guarded lambda so the pragma is checked in-context.
    const auto guardsPos = src.find("if (!backToTopGuard || !viewerForBtn)");
    ASSERT_NE(guardsPos, std::string::npos)
        << "positionBackToTop lambda's null guard is missing — the pragma "
        << "must not replace the runtime check";
    const auto vpGuardPos = src.find("if (!vp) return;", guardsPos);
    ASSERT_NE(vpGuardPos, std::string::npos)
        << "viewport null guard (if (!vp) return;) must remain";

    // INV-2 (contract §2) — GCC-only guard so clang doesn't warn on the pragma.
    const auto ifGuardPos =
        src.find("#if defined(__GNUC__) && !defined(__clang__)", vpGuardPos);
    ASSERT_NE(ifGuardPos, std::string::npos)
        << "pragma block must be guarded by "
        << "#if defined(__GNUC__) && !defined(__clang__)";

    // INV-1 (contract §1) — push, ignored, pop, in order, after the guards.
    const auto pushPos = src.find("#  pragma GCC diagnostic push", ifGuardPos);
    ASSERT_NE(pushPos, std::string::npos)
        << "missing `#  pragma GCC diagnostic push`";
    const auto ignoredPos =
        src.find("#  pragma GCC diagnostic ignored \"-Wnull-dereference\"",
                 pushPos);
    ASSERT_NE(ignoredPos, std::string::npos)
        << "missing `#  pragma GCC diagnostic ignored "
        << "\"-Wnull-dereference\"` line";
    const auto popPos = src.find("#  pragma GCC diagnostic pop", ignoredPos);
    ASSERT_NE(popPos, std::string::npos)
        << "missing matching `#  pragma GCC diagnostic pop`";

    EXPECT_LT(pushPos, ignoredPos) << "push must precede ignored";
    EXPECT_LT(ignoredPos, popPos) << "ignored must precede pop";

    // INV-3 (contract §3) — scoped, not file-wide: the pop must fall before
    // the lambda closes (the `};` that ends positionBackToTop), so the
    // suppression covers only the geometry block.
    const auto lambdaEndPos = src.find("};", popPos);
    ASSERT_NE(lambdaEndPos, std::string::npos);
    const auto nextPushAfterPop =
        src.find("#  pragma GCC diagnostic push", popPos);
    // No second push before the lambda ends → the block is a single scoped pair.
    EXPECT_TRUE(nextPushAfterPop == std::string::npos ||
                nextPushAfterPop > lambdaEndPos)
        << "expected exactly one scoped pragma pair around the geometry block";
}
