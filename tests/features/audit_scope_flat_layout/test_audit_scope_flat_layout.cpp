// ANTS-3741 — AuditScope::enumerateSourceFiles must not assume `src/`.
//
// The pathspec was hardcoded `git ls-files src/`, so a project keeping its
// code anywhere else (DOOM Ants in `linuxdoom-1.10/`, Fin Break flat at the
// root) enumerated NOTHING. runAudit then falls through
// `perToolPaths[tool] = safe.isEmpty() ? req.paths : safe` to an empty list,
// so every file-scoped tool reverted to its own whole-tree walk while
// `changed_files_count` was recorded as 0 — an envelope that says the
// opposite of what happened. It also silently broke the one thing
// scope:"full" was added for (ANTS-2015): real source positionals for
// clazy / clang-tidy.

#include "../../_support/expect.h"
#include "auditscope.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

bool haveGit() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

// ANTS-3841 — an inherited GIT_DIR / GIT_WORK_TREE makes a fixture repo's
// commands act on the REAL repository. Strip them rather than inheriting the
// caller's environment, so this test can never write to the project it is
// being run from.
bool runGit(const QString &dir, const QStringList &args) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("GIT_DIR"));
    env.remove(QStringLiteral("GIT_WORK_TREE"));
    env.remove(QStringLiteral("GIT_INDEX_FILE"));
    env.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    env.insert(QStringLiteral("GIT_CONFIG_SYSTEM"), QStringLiteral("/dev/null"));
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(3000)) return false;
    if (!p.waitForFinished(5000)) { p.kill(); return false; }
    return p.exitCode() == 0;
}

bool writeFile(const QString &dir, const QString &rel, const QString &body) {
    const QString abs = dir + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(body.toUtf8());
    return true;
}

bool initRepoWith(const QString &dir, const QStringList &relPaths) {
    if (!runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})) return false;
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t.test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("Test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("commit.gpgsign"),
                 QStringLiteral("false")});
    for (const QString &rel : relPaths)
        if (!writeFile(dir, rel, QStringLiteral("int x;\n"))) return false;
    return runGit(dir, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("seed")});
}

}  // namespace

// INV-1 — a FLAT-layout project (no src/) enumerates its tracked sources
// rather than an empty list. This is the regression: pre-fix it returned {}.
TEST(AuditScopeFlatLayout, Ants3741FlatProjectEnumeratesItsSources) {
    if (!haveGit()) GTEST_SKIP() << "git not on PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(initRepoWith(root, {QStringLiteral("main.c"),
                                    QStringLiteral("linuxdoom-1.10/r_draw.c"),
                                    QStringLiteral("README.md")}));

    const QStringList got = AuditScope::enumerateSourceFiles(root);
    EXPECT_FALSE(got.isEmpty())
        << "ANTS-3741: a project with no src/ must not enumerate zero files — "
           "an empty list makes every tool fall back to its own whole-tree "
           "walk while the envelope reports changed_files_count:0";
    EXPECT_TRUE(got.contains(QStringLiteral("main.c")));
    EXPECT_TRUE(got.contains(QStringLiteral("linuxdoom-1.10/r_draw.c")))
        << "ANTS-3741: code in a non-src directory is still this project's code";
}

// INV-2 — the src/ case is UNCHANGED: where src/ exists it still narrows to
// src/, so this fix widens only the layouts that were returning nothing.
TEST(AuditScopeFlatLayout, Ants3741SrcLayoutStillNarrowsToSrc) {
    if (!haveGit()) GTEST_SKIP() << "git not on PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(initRepoWith(root, {QStringLiteral("src/a.cpp"),
                                    QStringLiteral("src/b.h"),
                                    QStringLiteral("vendor/huge.c"),
                                    QStringLiteral("tools/script.c")}));

    const QStringList got = AuditScope::enumerateSourceFiles(root);
    EXPECT_TRUE(got.contains(QStringLiteral("src/a.cpp")));
    EXPECT_TRUE(got.contains(QStringLiteral("src/b.h")));
    EXPECT_FALSE(got.contains(QStringLiteral("vendor/huge.c")))
        << "ANTS-3741: a project WITH src/ must still narrow to it — widening "
           "that case would drag vendored trees into every full audit";
    EXPECT_FALSE(got.contains(QStringLiteral("tools/script.c")));
}
