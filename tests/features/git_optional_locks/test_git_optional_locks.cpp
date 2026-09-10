// ANTS-4999 — read-only git calls run with GIT_OPTIONAL_LOCKS=0, so a probe
// cannot hold .git/index.lock while a user's git commit needs it.
// See tests/features/git_optional_locks/spec.md.

#include "gitwrap.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QString>

#include <string>

namespace {

constexpr char kVar[] = "GIT_OPTIONAL_LOCKS";

// Clears the variable for one test and restores it after, so a parent
// environment that already sets it cannot make the test pass.
class OptionalLocksUnset {
public:
    OptionalLocksUnset()
        : m_had(qEnvironmentVariableIsSet(kVar)), m_saved(qgetenv(kVar)) {
        qunsetenv(kVar);
    }
    ~OptionalLocksUnset() {
        if (m_had) qputenv(kVar, m_saved);
    }
    OptionalLocksUnset(const OptionalLocksUnset &) = delete;
    OptionalLocksUnset &operator=(const OptionalLocksUnset &) = delete;

private:
    bool       m_had;
    QByteArray m_saved;
};

// The body of the function whose signature starts with `anchor`, comments
// stripped so a comment naming the helper cannot satisfy the check.
std::string codeOf(const std::string &src, const std::string &anchor) {
    return ants_test::stripComments(ants_test::slurpFunctionBody(src, anchor));
}

bool usesHelper(const std::string &code) {
    return code.find("readOnlyEnvironment()") != std::string::npos;
}

}  // namespace

// INV-1 — the helper sets the switch.
TEST(GitOptionalLocks, Inv1HelperDisablesOptionalLocks) {
    OptionalLocksUnset unset;
    EXPECT_EQ(GitWrap::readOnlyEnvironment().value(QString::fromLatin1(kVar)),
              QStringLiteral("0"));
}

// INV-2 — GitWrap::run hands it to git.
TEST(GitOptionalLocks, Inv2GitWrapRunHandsItToGit) {
    OptionalLocksUnset unset;
    const GitWrap::Result r = GitWrap::run(QDir::tempPath(),
        {QStringLiteral("-c"),
         QStringLiteral("alias.optlocks=!printf %s \"$GIT_OPTIONAL_LOCKS\""),
         QStringLiteral("optlocks")});
    if (!r.started) GTEST_SKIP() << "git not available on PATH";
    EXPECT_EQ(r.exitCode, 0) << r.stderrTail.constData();
    EXPECT_EQ(r.stdoutBytes, QByteArray("0"))
        << "git's child saw GIT_OPTIONAL_LOCKS=\"" << r.stdoutBytes.constData()
        << "\"";
}

// INV-3 — the runners that do not go through GitWrap::run use the helper.
TEST(GitOptionalLocks, Inv3EveryOtherReadOnlyRunnerUsesIt) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());

    const std::string probe =
        codeOf(mw, "void MainWindow::refreshReviewButton()");
    ASSERT_FALSE(probe.empty());
    EXPECT_TRUE(usesHelper(probe)) << "the Review button's git status probe";

    const std::string provider = ants_test::stripComments(
        ants_test::regionBetween(mw, "registerToolProvider(\"get_git_status\"",
                                 "registerToolProvider(\"get_environment\""));
    ASSERT_FALSE(provider.empty());
    EXPECT_TRUE(usesHelper(provider)) << "the get_git_status provider";

    const std::string rcRunGit = codeOf(ants_test::slurpRemoteControl(),
        "QByteArray runGit(const QString &root, const QStringList &argv) {");
    ASSERT_FALSE(rcRunGit.empty());
    EXPECT_TRUE(usesHelper(rcRunGit)) << "RemoteControl's shared runGit";

    const std::string scope = ants_test::slurpFile(SRC_AUDITSCOPE_CPP_PATH);
    ASSERT_FALSE(scope.empty());
    for (const char *anchor : {"QString runGit(const QString &root",
                               "QString runGitRaw(const QString &root",
                               "bool runGitSucceeds(const QString &root"}) {
        const std::string code = codeOf(scope, anchor);
        ASSERT_FALSE(code.empty()) << anchor;
        EXPECT_TRUE(usesHelper(code)) << "auditscope.cpp " << anchor;
    }
}

// INV-4 — the git-context hook script's status call skips optional locks.
TEST(GitOptionalLocks, Inv4HookScriptStatusSkipsOptionalLocks) {
    const std::string hook = ants_test::slurpFunctionBody(
        ants_test::slurpFile(SRC_SETTINGSDIALOG_CPP_PATH),
        "void SettingsDialog::installClaudeGitContextHook()");
    ASSERT_FALSE(hook.empty());
    EXPECT_NE(hook.find("GIT_OPTIONAL_LOCKS=0 git status"), std::string::npos);
}
