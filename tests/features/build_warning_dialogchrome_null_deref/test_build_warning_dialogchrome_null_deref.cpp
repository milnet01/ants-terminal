// Feature-conformance test for spec.md (ANTS-3358).
//
// Verifies the GCC-only scoped pragma suppressing the ~33
// -Wnull-dereference false positives from DialogChrome::ChromeGuard's
// geometry accessors is present, guarded, and correctly bracketed.
// Static source-grep only (mirrors the ANTS-1554 fixture); the actual
// "0 warnings" property was verified by an -O3 compile when the fix landed.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

TEST(BuildWarningDialogChromeNullDeref, PragmaGuardPresent) {
    const std::string src = ants_test::slurpFile(SRC_DIALOGCHROME_CPP);
    ASSERT_FALSE(src.empty()) << "could not read " << SRC_DIALOGCHROME_CPP;

    // INV-4 (contract §4) — the runtime null guards the pragma stands in
    // for must still be present; the pragma silences the diagnostic, not
    // the check.
    EXPECT_NE(src.find("if (!m_dlg) return;"), std::string::npos)
        << "recenter()/positionGrip() must keep their null-m_dlg guard";

    // INV-2 (contract §2) — the pragma block opens behind a GCC-only guard
    // so clang builds don't warn on an unknown pragma.
    const auto guardPos =
        src.find("#if defined(__GNUC__) && !defined(__clang__)");
    ASSERT_NE(guardPos, std::string::npos)
        << "pragma block must be guarded by "
        << "#if defined(__GNUC__) && !defined(__clang__)";

    // INV-1 (contract §1) — matching push, ignored, pop, in that order.
    const auto pushPos = src.find("#  pragma GCC diagnostic push", guardPos);
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

    // INV-3 (contract §3) — the suppression is scoped, not file-wide: the
    // pop must fall before the ChromeGuard members close the class, i.e.
    // it wraps the method cluster rather than the whole TU.
    const auto membersPos = src.find("QPointer<QDialog> m_dlg;");
    ASSERT_NE(membersPos, std::string::npos)
        << "expected the m_dlg member declaration as the scope end-marker";
    EXPECT_LT(popPos, membersPos)
        << "pragma pop must close before the class members — suppression "
        << "must stay scoped to the method cluster";

    // The whole block lives inside ChromeGuard's method region: push must
    // come after the class's `private:` label, not at file scope.
    const auto privatePos = src.rfind("private:", pushPos);
    ASSERT_NE(privatePos, std::string::npos);
    EXPECT_LT(privatePos, pushPos)
        << "pragma push must sit inside the ChromeGuard method cluster";
}
