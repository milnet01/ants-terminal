// Feature-conformance test for tests/features/status_bar_branch_chip/spec.md.
//
// Locks ANTS-1109 — git-branch chip styled to match the repo-
// visibility pill, with green/amber color derived from the branch
// name. Drives the pure helper branchchip::isPrimaryBranch +
// source-greps mainwindow.cpp for the consultation site and the
// shape match against the visibility pill.
//
// Exit 0 = all invariants hold.

#include "branchchip.h"
#include "gitrepo.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// ANTS-1589 — report failures at the CALL site, not this helper's line.
// `failAt` takes file/line params; the `fail(...)` macro below injects the
// caller's __FILE__/__LINE__ so ctest output points at the real assertion.
void failAt(const char *file, int line, const char *label, const char *why) {
    ADD_FAILURE_AT(file, line) << "[" << label << "] " << why;
}
#define fail(label, why) failAt(__FILE__, __LINE__, (label), (why))

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(bytes);
}

}  // namespace

// INV-9 (ANTS-6187) — the chip finds the branch the way the visibility badge
// finds the repo: a worktree's `.git` is a FILE whose gitdir holds the HEAD.
// Laid out by hand as `git worktree add` leaves it, so no git binary runs.
TEST(StatusBarBranchChip, Inv9WorktreeBranchIsRead) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString main = tmp.filePath(QStringLiteral("main"));
    const QString wt = tmp.filePath(QStringLiteral("wt"));
    writeFile(main + QStringLiteral("/.git/HEAD"), "ref: refs/heads/main\n");
    writeFile(main + QStringLiteral("/.git/worktrees/wt/HEAD"), "ref: refs/heads/feature-x\n");
    writeFile(wt + QStringLiteral("/.git"), "gitdir: ../main/.git/worktrees/wt\n");
    QDir().mkpath(main + QStringLiteral("/src/deep"));
    QDir().mkpath(wt + QStringLiteral("/src/deep"));

    EXPECT_EQ(QStringLiteral("main"), GitRepo::branchOf(main + QStringLiteral("/src/deep")));
    EXPECT_EQ(QStringLiteral("feature-x"), GitRepo::branchOf(wt + QStringLiteral("/src/deep")))
        << "a worktree's branch was not read through its .git file";
    // The badge and the chip agree on the root.
    EXPECT_EQ(QDir(wt).absolutePath(), GitRepo::rootOf(wt + QStringLiteral("/src/deep")));

    // A detached worktree shows the short commit, as a detached checkout does.
    writeFile(main + QStringLiteral("/.git/worktrees/wt/HEAD"),
              "0123456789abcdef0123456789abcdef01234567\n");
    EXPECT_EQ(QStringLiteral("0123456"), GitRepo::branchOf(wt));
    EXPECT_TRUE(GitRepo::branchOf(tmp.path()).isEmpty());
}

// INV-10 (ANTS-6187) — both status-bar sites use the one finder.
TEST(StatusBarBranchChip, Inv10OneFinderForChipAndBadge) {
    const std::string mw = ants_test::slurpMainWindow();
    EXPECT_TRUE(contains(mw, "GitRepo::branchOf(")) << "the branch chip has its own walk";
    EXPECT_TRUE(contains(mw, "GitRepo::rootOf(")) << "the visibility badge has its own walk";
    EXPECT_FALSE(contains(mw, "\"/.git/HEAD\"")) << "mainwindow still reads .git/HEAD itself";
}

TEST(StatusBarBranchChip, Main) {
    // INV-1..3: the three primary-branch names return true.
    if (!branchchip::isPrimaryBranch(QStringLiteral("main")))
        fail("INV-1", "main should be primary");
    if (!branchchip::isPrimaryBranch(QStringLiteral("master")))
        fail("INV-2", "master should be primary");
    if (!branchchip::isPrimaryBranch(QStringLiteral("trunk")))
        fail("INV-3", "trunk should be primary");

    // INV-4: a feature branch is not primary.
    if (branchchip::isPrimaryBranch(QStringLiteral("feature/foo")))
        fail("INV-4", "feature/foo should not be primary");

    // INV-5: empty is not primary.
    if (branchchip::isPrimaryBranch(QStringLiteral("")))
        fail("INV-5", "empty branch should not be primary");

    // INV-6: case-sensitive match — MAIN is not the same as main.
    if (branchchip::isPrimaryBranch(QStringLiteral("MAIN")))
        fail("INV-6", "MAIN should not be primary (case-sensitive)");

    // INV-7: mainwindow.cpp consults the helper.
    const std::string source = ants_test::slurpMainWindow();
    if (source.empty()) fail("setup", "mainwindow.cpp not readable");
    if (!contains(source, "branchchip::isPrimaryBranch"))
        fail("INV-7",
            "mainwindow.cpp does not call branchchip::isPrimaryBranch");

    // INV-8: every branch-chip setStyleSheet site is wired off
    // branchchip::isPrimaryBranch and uses theme.ansi[2] /
    // theme.ansi[3] (the palette roles the visibility pill picks).
    // Re-targeted from the original "shape substrings" check
    // because today's branch-chip stylesheet *already* contains
    // the shape literals (cold-eyes F8). Scans ALL setStyleSheet
    // matches because the chip is restyled from two paths —
    // applyTheme (theme switch) and updateStatusBar (per-poll
    // refresh on branch change). A future regression that drops
    // the helper from one path while keeping the other would
    // silently break the user-visible cue (cold-eyes F1).
    {
        const std::string needle = "m_statusGitBranch->setStyleSheet(";
        size_t pos = 0;
        int sites = 0;
        while ((pos = source.find(needle, pos)) != std::string::npos) {
            ++sites;
            const size_t windowStart = pos > 400 ? pos - 400 : 0;
            const std::string region = source.substr(windowStart,
                pos - windowStart + 600);
            if (!contains(region, "branchchip::isPrimaryBranch"))
                fail("INV-8",
                    "branch chip setStyleSheet site does not consult branchchip::isPrimaryBranch");
            if (!contains(region, "ansi[2]"))
                fail("INV-8",
                    "branch chip setStyleSheet site does not reference theme.ansi[2] (primary green)");
            if (!contains(region, "ansi[3]"))
                fail("INV-8",
                    "branch chip setStyleSheet site does not reference theme.ansi[3] (feature amber)");
            pos += needle.size();
        }
        if (sites < 2)
            fail("INV-8",
                "expected ≥ 2 branch-chip setStyleSheet sites (applyTheme + updateStatusBar restyle)");
    }

    std::fprintf(stderr,
        "OK — status bar branch chip styling INVs hold.\n");
    return;
}

