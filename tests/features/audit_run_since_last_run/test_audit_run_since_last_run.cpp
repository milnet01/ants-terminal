// Feature-conformance test for spec.md — ANTS-1504 audit_run
// since-last-run scope + the shared narrowing resolver (auditscope).
//
// Pure helpers are exercised directly; resolveChangedFiles is exercised
// against a real git repo in a QTemporaryDir; the runAudit/envelope wiring
// is pinned by source-scrape. GUI-free (no QCoreApplication needed).

#include "auditscope.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include "support/testspawn.h"

#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace {

// ── git fixture helpers ───────────────────────────────────────────────

bool gitAvailable() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

bool runGit(const QString &dir, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    if (!ants_test::waitForHelper(p)) return false;   // ANTS-4651
    return p.exitCode() == 0;
}

QString gitOut(const QString &dir, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    if (!ants_test::waitForHelper(p)) return {};      // ANTS-4651
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

bool initRepo(const QString &dir) {
    if (!runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})) return false;
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t.test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("Test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("commit.gpgsign"),
                 QStringLiteral("false")});
    return true;
}

// ANTS-2063 — bool return so a failed open fails the TEST at the call
// site, not silently inside the helper.
bool writeFile(const QString &dir, const QString &rel, const QString &body) {
    const QString abs = dir + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(body.toUtf8());
    f.close();
    return true;
}

bool commitAll(const QString &dir, const QString &msg) {
    return runGit(dir, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), msg});
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── INV-4 — repo-global tools are not file-scoped ─────────────────────

TEST(AuditScopeSinceLastRun, Inv4FileScopedClassification) {
    EXPECT_FALSE(AuditScope::isFileScopedTool(QStringLiteral("gitleaks")));
    EXPECT_FALSE(AuditScope::isFileScopedTool(QStringLiteral("trivy")));
    for (const char *t : {"cppcheck", "clazy", "clang-tidy", "ruff",
                          "bandit", "mypy", "shellcheck", "semgrep"})
        EXPECT_TRUE(AuditScope::isFileScopedTool(QString::fromLatin1(t))) << t;
}

// ── INV-3 — per-tool language filter ──────────────────────────────────

TEST(AuditScopeSinceLastRun, Inv3LanguageFilter) {
    const QStringList files = {QStringLiteral("src/a.cpp"),
                               QStringLiteral("src/b.h"),
                               QStringLiteral("tool.py"),
                               QStringLiteral("run.sh"),
                               QStringLiteral("README.md")};

    const auto cpp = AuditScope::filterForTool(files, QStringLiteral("cppcheck"));
    EXPECT_TRUE(cpp.contains(QStringLiteral("src/a.cpp")));
    EXPECT_TRUE(cpp.contains(QStringLiteral("src/b.h")));
    EXPECT_FALSE(cpp.contains(QStringLiteral("tool.py")));
    EXPECT_FALSE(cpp.contains(QStringLiteral("README.md")));

    const auto py = AuditScope::filterForTool(files, QStringLiteral("ruff"));
    EXPECT_EQ(py, QStringList{QStringLiteral("tool.py")});

    const auto sh = AuditScope::filterForTool(files, QStringLiteral("shellcheck"));
    EXPECT_EQ(sh, QStringList{QStringLiteral("run.sh")});

    // semgrep self-selects → receives every changed file.
    EXPECT_EQ(AuditScope::filterForTool(files, QStringLiteral("semgrep")), files);
}

// ── INV-1 — parseChangedFiles (pure) ──────────────────────────────────

TEST(AuditScopeSinceLastRun, Inv1ParseChangedFiles) {
    const QString diff = QStringLiteral("src/a.cpp\nsrc/b.cpp\n");
    const QString status = QStringLiteral(
        " M src/a.cpp\n"      // modified (dup of diff → deduped)
        "?? newfile.py\n"     // untracked → kept
        " D gone.cpp\n"       // deleted → dropped
        "R  old.cpp -> new.cpp\n");  // rename → new path

    const auto withWt = AuditScope::parseChangedFiles(diff, status, true);
    EXPECT_TRUE(withWt.contains(QStringLiteral("src/a.cpp")));
    EXPECT_TRUE(withWt.contains(QStringLiteral("src/b.cpp")));
    EXPECT_TRUE(withWt.contains(QStringLiteral("newfile.py")));
    EXPECT_TRUE(withWt.contains(QStringLiteral("new.cpp")));
    EXPECT_FALSE(withWt.contains(QStringLiteral("gone.cpp")));
    EXPECT_FALSE(withWt.contains(QStringLiteral("old.cpp")));
    // dedup: a.cpp appears once.
    EXPECT_EQ(withWt.count(QStringLiteral("src/a.cpp")), 1);

    // includeWorkingTree=false → only the diff lines.
    const auto noWt = AuditScope::parseChangedFiles(diff, status, false);
    EXPECT_EQ(noWt, (QStringList{QStringLiteral("src/a.cpp"),
                                 QStringLiteral("src/b.cpp")}));
}

// ── INV-6 — demotion reasons (no git needed for the string cases) ─────

TEST(AuditScopeSinceLastRun, Inv6DemotionNoGitAndNoPrior) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Not a git work tree → not_git for a narrowing scope.
    auto r = AuditScope::resolveChangedFiles(tmp.path(),
        QStringLiteral("since-last-run"), QStringLiteral("abc1234"));
    EXPECT_TRUE(r.narrowed);
    EXPECT_EQ(r.demotedReason, QStringLiteral("not_git"));

    if (!gitAvailable()) GTEST_SKIP() << "git not on PATH";
    QTemporaryDir repo;
    ASSERT_TRUE(repo.isValid());
    ASSERT_TRUE(initRepo(repo.path()));
    ASSERT_TRUE(writeFile(repo.path(), QStringLiteral("a.cpp"), QStringLiteral("int x;\n")));
    ASSERT_TRUE(commitAll(repo.path(), QStringLiteral("init")));

    // empty prior commit → no_prior_run.
    EXPECT_EQ(AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("since-last-run"), QString()).demotedReason,
        QStringLiteral("no_prior_run"));
    // "nogit" prior commit → no_prior_commit.
    EXPECT_EQ(AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("since-last-run"), QStringLiteral("nogit")).demotedReason,
        QStringLiteral("no_prior_commit"));
    // unreachable commit → prior_commit_unreachable.
    EXPECT_EQ(AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("since-last-run"),
        QStringLiteral("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")).demotedReason,
        QStringLiteral("prior_commit_unreachable"));
    // files scope, single root commit, no origin → no_merge_base.
    EXPECT_EQ(AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("files"), QString()).demotedReason,
        QStringLiteral("no_merge_base"));
    // auto → not narrowed.
    EXPECT_FALSE(AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("auto"), QString()).narrowed);
}

// ── INV-1/INV-7 — resolve against a real repo ─────────────────────────

TEST(AuditScopeSinceLastRun, Inv1ResolveDiffAndInv7NoChanges) {
    if (!gitAvailable()) GTEST_SKIP() << "git not on PATH";
    QTemporaryDir repo;
    ASSERT_TRUE(repo.isValid());
    ASSERT_TRUE(initRepo(repo.path()));
    ASSERT_TRUE(writeFile(repo.path(), QStringLiteral("a.cpp"), QStringLiteral("int x;\n")));
    ASSERT_TRUE(commitAll(repo.path(), QStringLiteral("A")));
    const QString anchor = gitOut(repo.path(),
        {QStringLiteral("rev-parse"), QStringLiteral("--short"),
         QStringLiteral("HEAD")});
    ASSERT_FALSE(anchor.isEmpty());

    // INV-7: clean tree at HEAD == anchor → noChanges, no demotion.
    auto clean = AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("since-last-run"), anchor);
    EXPECT_TRUE(clean.narrowed);
    EXPECT_TRUE(clean.demotedReason.isEmpty());
    EXPECT_TRUE(clean.noChanges);
    EXPECT_TRUE(clean.files.isEmpty());
    EXPECT_EQ(clean.anchorCommit, anchor);

    // Commit a second change + leave an uncommitted edit + an untracked file.
    ASSERT_TRUE(writeFile(repo.path(), QStringLiteral("b.cpp"), QStringLiteral("int y;\n")));
    ASSERT_TRUE(commitAll(repo.path(), QStringLiteral("B")));
    ASSERT_TRUE(writeFile(repo.path(), QStringLiteral("a.cpp"), QStringLiteral("int x=1;\n")));
    ASSERT_TRUE(writeFile(repo.path(), QStringLiteral("c.py"), QStringLiteral("z=1\n")));

    auto r = AuditScope::resolveChangedFiles(repo.path(),
        QStringLiteral("since-last-run"), anchor);
    EXPECT_TRUE(r.narrowed);
    EXPECT_TRUE(r.demotedReason.isEmpty());
    EXPECT_FALSE(r.noChanges);
    // committed-since-anchor: b.cpp; working-tree: a.cpp (mod) + c.py (untracked).
    EXPECT_TRUE(r.files.contains(QStringLiteral("b.cpp")));
    EXPECT_TRUE(r.files.contains(QStringLiteral("a.cpp")));
    EXPECT_TRUE(r.files.contains(QStringLiteral("c.py")));
}

// ── INV-9 — pure/impure split (source-scrape) ─────────────────────────

TEST(AuditScopeSinceLastRun, Inv9PureHelpersDoNotSpawnGit) {
    const std::string src = ants_test::slurpFile(SRC_AUDITSCOPE_CPP_PATH);
    ASSERT_FALSE(src.empty());
    // The pure helpers live between isFileScopedTool and resolveChangedFiles;
    // none of them may call git.
    const auto pureStart = src.find("bool isFileScopedTool");
    const auto impure = src.find("Resolution resolveChangedFiles");
    ASSERT_NE(pureStart, std::string::npos);
    ASSERT_NE(impure, std::string::npos);
    ASSERT_LT(pureStart, impure);
    const std::string pureRegion = src.substr(pureStart, impure - pureStart);
    EXPECT_FALSE(has(pureRegion, "runGit"))
        << "pure helpers must not call git";
    EXPECT_FALSE(has(pureRegion, "QProcess"))
        << "pure helpers must not spawn a process";
    // The impure resolver does shell out.
    EXPECT_TRUE(has(src.substr(impure), "runGit"));
}

// ── INV-2/INV-5 — runAudit wires the resolver + path-safety drop ──────

TEST(AuditScopeSinceLastRun, Inv2And5RunAuditWiring) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(has(src, "AuditScope::resolveChangedFiles"))
        << "runAudit must call the narrowing resolver";
    EXPECT_TRUE(has(src, "isFileScopedTool"))
        << "repo-global tools must be classified";
    EXPECT_TRUE(has(src, "not_file_scoped"));
    EXPECT_TRUE(has(src, "no_changed_files_for_languages"));
    // INV-5: git-derived paths re-validated before argv.
    EXPECT_TRUE(has(src, "isAuditArgSafe(p)"))
        << "scoped paths must pass isAuditArgSafe before argv";
}

// ── INV-8 — envelope surfaces the scope fields (source-scrape) ────────

TEST(AuditScopeSinceLastRun, Inv8EnvelopeFields) {
    const std::string src = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(src.empty());
    for (const char *key : {"scope_resolved", "changed_files_count",
                            "scope_anchor_commit", "scope_demoted",
                            "scope_demoted_reason", "no_changes"})
        EXPECT_TRUE(has(src, key)) << key;
}
