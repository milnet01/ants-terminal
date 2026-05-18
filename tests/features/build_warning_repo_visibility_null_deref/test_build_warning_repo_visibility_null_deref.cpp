// Feature-conformance test for spec.md (ANTS-1554).
//
// Verifies the tightly-scoped GCC pragma suppressing the -Wnull-dereference
// false positive in MainWindow::refreshRepoVisibility()'s QProcess::finished
// lambda is present + correctly bracketed.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(BuildWarningRepoVisibilityNullDeref, PragmaGuardPresent) {
    const std::string src = slurp(SRC_MAINWINDOW_CPP);
    ASSERT_FALSE(src.empty()) << "could not read " << SRC_MAINWINDOW_CPP;

    // INV-1 — the .remove(repoRoot) call must still be present.
    const std::string kRemove =
        "m_repoVisibilityProbeInFlight.remove(repoRoot)";
    const auto removePos = src.find(kRemove);
    ASSERT_NE(removePos, std::string::npos)
        << "missing in-flight tracker remove() call — refresh path "
        << "would leak entries on completion";

    // Window around the call: 600 chars before to capture the full
    // pragma block + 100 after for the pop.
    const std::size_t windowStart =
        removePos > 600 ? removePos - 600 : 0;
    const std::size_t windowLen =
        std::min<std::size_t>(700, src.size() - windowStart);
    const std::string window = src.substr(windowStart, windowLen);

    // INV-4 — the pragma block opens behind a GCC-only guard so clang
    // builds don't see an unknown-pragma warning.
    EXPECT_NE(window.find("#if defined(__GNUC__) && !defined(__clang__)"),
              std::string::npos)
        << "pragma push block must be guarded by "
        << "#if defined(__GNUC__) && !defined(__clang__)";

    // INV-3 — matching push must precede the ignored line.
    const auto pushPos = window.find("#  pragma GCC diagnostic push");
    EXPECT_NE(pushPos, std::string::npos)
        << "missing `#  pragma GCC diagnostic push` before remove()";

    // INV-2 — the ignored line must name the exact diagnostic id.
    const auto ignoredPos =
        window.find("#  pragma GCC diagnostic ignored \"-Wnull-dereference\"");
    EXPECT_NE(ignoredPos, std::string::npos)
        << "missing `#  pragma GCC diagnostic ignored "
        << "\"-Wnull-dereference\"` line";

    // INV-3 — the pop must come AFTER the .remove() call.
    const auto popPos = src.find("#  pragma GCC diagnostic pop", removePos);
    EXPECT_NE(popPos, std::string::npos)
        << "missing matching `#  pragma GCC diagnostic pop` after remove()";

    // Order check: push < ignored < remove < pop within the same block.
    if (pushPos != std::string::npos && ignoredPos != std::string::npos) {
        EXPECT_LT(pushPos, ignoredPos)
            << "push must precede ignored";
    }
    const std::size_t removeAbs = removePos;
    if (ignoredPos != std::string::npos && popPos != std::string::npos) {
        const std::size_t ignoredAbs = windowStart + ignoredPos;
        EXPECT_LT(ignoredAbs, removeAbs);
        EXPECT_LT(removeAbs, popPos);
    }
}
