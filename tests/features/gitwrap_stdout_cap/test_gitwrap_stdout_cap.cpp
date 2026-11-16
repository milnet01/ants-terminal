// Feature-conformance test for spec.md (ANTS-1839).
//
// Behavioral: drives GitWrap::run with `git --version` (no repo needed) and a
// tiny stdout budget. Skips gracefully when git is not on PATH.

#include "gitwrap.h"
#include "../../_support/peak_memory.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

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

// INV-3 (ANTS-6512) — the cap bounds memory, not just the bytes returned:
// stdout is read as git writes it, so a 48 MiB diff held to a 64 KiB cap
// never sits whole in the process.
TEST(GitWrapStdoutCap, CapBoundsPeakMemory) {
    ANTS_TEST_SKIP_UNDER_ASAN();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString big = tmp.filePath(QStringLiteral("big.txt"));
    {
        QFile f(big);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        const QByteArray line(63, 'x');
        const QByteArray block = (line + '\n').repeated(16 * 1024);  // 1 MiB
        for (int i = 0; i < 48; ++i) ASSERT_EQ(f.write(block), block.size());
    }
    const QStringList argv{QStringLiteral("diff"), QStringLiteral("--no-index"),
                           QStringLiteral("--no-color"), QStringLiteral("/dev/null"), big};
    constexpr int kCap = 64 * 1024;
    GitWrap::Result probe = GitWrap::runWithin(tmp.path(), {QStringLiteral("--version")},
                                               kCap, 5000, false);
    if (!probe.started) GTEST_SKIP() << "git not available on PATH";

    const int growth = ants_test::peakGrowthMiB([&] {
        const GitWrap::Result r = GitWrap::runWithin(tmp.path(), argv, kCap, 60000, false);
        return r.started && !r.hardKilled && r.stdoutTruncated
               && r.stdoutBytes.size() == kCap;
    });
    ASSERT_GE(growth, 0) << "fork failed";
    ASSERT_NE(growth, 251) << "the diff did not run, or was not truncated to the cap";
    EXPECT_LT(growth, 16) << "peak memory rose " << growth << " MiB for a 64 KiB cap";
}
