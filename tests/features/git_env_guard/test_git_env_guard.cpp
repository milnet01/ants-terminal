// ANTS-3841 — a test run must not be able to write to the developer's real
// repository. See tests/features/git_env_guard/spec.md.
#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdlib>

namespace {

// The set tests/_support/git_env_guard.h scrubs. Kept in step with it by
// INV-3, which asserts the scrub is scoped to redirecting variables only.
const char *const kRedirectingVars[] = {
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_INDEX_FILE",
    "GIT_COMMON_DIR",
};

bool gitAvailable()
{
    QProcess p;
    p.start(QStringLiteral("git"), {QStringLiteral("--version")});
    return p.waitForStarted(5000) && p.waitForFinished(10000)
           && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

}  // namespace

// INV-1 — nothing that redirects git survives into a test process. The
// polluted ctest entry (git_env_guard_polluted) exports all four, so this
// assertion is only meaningful there; the ordinary entry runs it clean.
TEST(GitEnvGuard, NoRedirectingGitVariableSurvivesIntoTheTestProcess)
{
    for (const char *name : kRedirectingVars) {
        const char *value = ::getenv(name);
        EXPECT_EQ(value, nullptr)
            << name << " reached the test process as \""
            << (value ? value : "") << "\". A git-fixture test would operate "
            << "on that repository instead of its own fixture — the ANTS-3841 "
            << "case that rewrote local main. Scrub it in "
            << "tests/_support/git_env_guard.h.";
    }
}

// INV-2 — the behaviour INV-1 protects. Verified 2026-09-09: with GIT_DIR set,
// `git -C <fixture> init` builds the repository at GIT_DIR and leaves
// <fixture>/.git absent, so this fails on its own if the scrub is removed.
TEST(GitEnvGuard, GitInitLandsInsideTheFixtureNotWhereTheCallerPointed)
{
    if (!gitAvailable())
        GTEST_SKIP() << "git not in PATH";

    QTemporaryDir fixture;
    ASSERT_TRUE(fixture.isValid());

    QProcess git;
    git.setWorkingDirectory(fixture.path());
    git.start(QStringLiteral("git"),
              {QStringLiteral("-C"), fixture.path(), QStringLiteral("init"),
               QStringLiteral("-q")});
    ASSERT_TRUE(git.waitForStarted(5000));
    ASSERT_TRUE(git.waitForFinished(30000));
    ASSERT_EQ(git.exitCode(), 0)
        << "git init failed: " << git.readAllStandardError().toStdString();

    EXPECT_TRUE(QFileInfo::exists(fixture.filePath(QStringLiteral(".git"))))
        << "git init did not create its repository inside the fixture. An "
        << "inherited GIT_DIR redirected it, which is exactly how a test run "
        << "reaches the developer's real history (ANTS-3841).";
}

// INV-3 — the scrub stays scoped. A variable that only changes git's behaviour
// INSIDE the repository it was already given cannot send a write elsewhere, so
// scrubbing it would be unexplained scope rather than safety. Identity is set
// by each fixture, so clearing it here would break nothing and prove nothing.
TEST(GitEnvGuard, ScrubIsScopedToRepositoryRedirectingVariables)
{
    EXPECT_EQ(std::size(kRedirectingVars), 4u)
        << "The scrubbed set changed. Adding a variable is fine if it "
        << "redirects WHICH repository git acts on; if it does not, it does "
        << "not belong in this guard (spec.md INV-3).";
}
