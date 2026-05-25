// Feature-conformance test for spec.md (ANTS-1839).
//
// Behavioral: drives GitWrap::run with `git --version` (no repo needed) and a
// tiny stdout budget. Skips gracefully when git is not on PATH.

#include "gitwrap.h"

#include <gtest/gtest.h>

#include <QDir>

// INV-1 — output longer than the cap is truncated + flagged.
TEST(GitWrapStdoutCap, TruncatesAtCap) {
    GitWrap::Result r = GitWrap::run(QDir::tempPath(),
                                     {QStringLiteral("--version")}, 8);
    if (!r.started) GTEST_SKIP() << "git not available on PATH";
    EXPECT_TRUE(r.stdoutTruncated);
    EXPECT_EQ(r.stdoutBytes.size(), 8);
}

// INV-2 — a cap larger than the output truncates nothing.
TEST(GitWrapStdoutCap, NoTruncationUnderCap) {
    GitWrap::Result r = GitWrap::run(QDir::tempPath(),
                                     {QStringLiteral("--version")},
                                     1 * 1024 * 1024);
    if (!r.started) GTEST_SKIP() << "git not available on PATH";
    EXPECT_FALSE(r.stdoutTruncated);
    EXPECT_GT(r.stdoutBytes.size(), 0);
}
