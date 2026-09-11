// ANTS-5040 — source-scrape + real-git regression test. See spec.md.
//
// Two invariants, both expected to fail against the current tree:
//   INV-1 — GitBlame::parseLinePorcelain (src/gitblameparse.h) must parse a
//           real `git blame --line-porcelain` invocation covering several
//           `-L` ranges, across commits. It ships as a stub returning {}.
//   INV-2 — the blame path reachable from AuditDialog::renderResults() must
//           not block the GUI thread on `git blame`, and must batch several
//           lines into one process per file. enrichWithBlame() still calls
//           QProcess::waitForFinished(2000) once per finding today, and the
//           file has no `--line-porcelain` invocation at all.
//
// AuditDialog is a QDialog; this project's house pattern for its
// invariants is source-scrape (see audit_dialog_render_hardening,
// audit_tool_process_group_kill), not construction — INV-2 below follows
// that pattern. INV-1 drives the parser directly since it needs no QDialog.

#include <gtest/gtest.h>
#include "gitblameparse.h"
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <string>

namespace {

bool gitOnPath() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

// Run a git subcommand in `cwd`, optionally with extra env vars layered
// over the system environment. Returns true on exit-0; stdout is written
// to *out when non-null.
bool runGit(const QString &cwd, const QStringList &args,
            const QProcessEnvironment &extraEnv = QProcessEnvironment(),
            QByteArray *out = nullptr) {
    QProcess git;
    git.setWorkingDirectory(cwd);
    if (!extraEnv.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &key : extraEnv.keys()) env.insert(key, extraEnv.value(key));
        git.setProcessEnvironment(env);
    }
    git.start(QStringLiteral("git"), args);
    if (!git.waitForStarted(5000)) return false;
    // Generous bound — a cold-cache git on a loaded runner can be slow, and
    // this timing out looks exactly like the fixture being wrong.
    if (!git.waitForFinished(30000)) { git.kill(); return false; }
    if (out) *out = git.readAllStandardOutput();
    return git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0;
}

QString writeFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return {};
    QTextStream out(&f);
    out << content;
    f.close();
    return path;
}

}  // namespace

// ---------------------------------------------------------------------
// INV-1 — the parser.
// ---------------------------------------------------------------------

TEST(AuditBlameBulkAsync, LinePorcelainParserCoversAllRequestedLinesAcrossCommits) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    QTemporaryDir fixture;
    ASSERT_TRUE(fixture.isValid());
    const QString dir = fixture.path();

    ASSERT_TRUE(runGit(dir, {QStringLiteral("init"),
                             QStringLiteral("--initial-branch=main")}))
        << "git init failed";
    ASSERT_TRUE(runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                             QStringLiteral("Blame Tester")}));
    ASSERT_TRUE(runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                             QStringLiteral("blame@example.com")}));

    const QString filePath = dir + QStringLiteral("/blamed.txt");

    // Commit 1 — 10 lines, all authored at epoch1.
    QStringList v1;
    for (int i = 1; i <= 10; ++i) v1 << QStringLiteral("line %1").arg(i);
    ASSERT_FALSE(writeFile(filePath, v1.join('\n') + QStringLiteral("\n")).isEmpty());
    ASSERT_TRUE(runGit(dir, {QStringLiteral("add"), QStringLiteral("blamed.txt")}));

    const qint64 epoch1 = QDateTime::fromString(QStringLiteral("2020-01-01T12:00:00Z"),
                                                 Qt::ISODate).toSecsSinceEpoch();
    ASSERT_GT(epoch1, 0);
    QProcessEnvironment env1;
    // Raw "<epoch> <tz>" form — the same shape `git log --date=raw` prints,
    // and the shape author-time/author-tz land in --line-porcelain output.
    env1.insert(QStringLiteral("GIT_AUTHOR_DATE"),
                QStringLiteral("%1 +0000").arg(epoch1));
    env1.insert(QStringLiteral("GIT_COMMITTER_DATE"),
                QStringLiteral("%1 +0000").arg(epoch1));
    ASSERT_TRUE(runGit(dir, {QStringLiteral("commit"), QStringLiteral("-m"),
                             QStringLiteral("commit 1")}, env1))
        << "commit 1 failed";

    QByteArray sha1Out;
    ASSERT_TRUE(runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--short=8"),
                             QStringLiteral("HEAD")}, {}, &sha1Out));
    const QString sha1 = QString::fromUtf8(sha1Out).trimmed();
    ASSERT_EQ(sha1.size(), 8) << "unexpected short-sha length for commit 1";

    // Commit 2 — change line 5 only, at a later epoch. Two different shas
    // now appear across the ranges we'll blame below (line 5 vs lines 2/9),
    // exercising --line-porcelain's per-line header repetition.
    QStringList v2 = v1;
    v2[4] = QStringLiteral("line 5 (changed)");
    ASSERT_FALSE(writeFile(filePath, v2.join('\n') + QStringLiteral("\n")).isEmpty());
    ASSERT_TRUE(runGit(dir, {QStringLiteral("add"), QStringLiteral("blamed.txt")}));

    const qint64 epoch2 = epoch1 + 30 * 86400;  // one month later
    QProcessEnvironment env2;
    env2.insert(QStringLiteral("GIT_AUTHOR_DATE"),
                QStringLiteral("%1 +0000").arg(epoch2));
    env2.insert(QStringLiteral("GIT_COMMITTER_DATE"),
                QStringLiteral("%1 +0000").arg(epoch2));
    ASSERT_TRUE(runGit(dir, {QStringLiteral("commit"), QStringLiteral("-m"),
                             QStringLiteral("commit 2")}, env2))
        << "commit 2 failed";

    QByteArray sha2Out;
    ASSERT_TRUE(runGit(dir, {QStringLiteral("rev-parse"), QStringLiteral("--short=8"),
                             QStringLiteral("HEAD")}, {}, &sha2Out));
    const QString sha2 = QString::fromUtf8(sha2Out).trimmed();
    ASSERT_EQ(sha2.size(), 8) << "unexpected short-sha length for commit 2";
    ASSERT_NE(sha1, sha2) << "test setup: the two commits collapsed to one sha";

    // The date string the *implementation's own conversion rule* would
    // produce for each epoch (QDateTime::fromSecsSinceEpoch(...).toString(
    // "yyyy-MM-dd"), local time) — computed here rather than hardcoded, so
    // the assertion doesn't depend on the host's timezone.
    const QString expectedDate1 =
        QDateTime::fromSecsSinceEpoch(epoch1).toString(QStringLiteral("yyyy-MM-dd"));
    const QString expectedDate2 =
        QDateTime::fromSecsSinceEpoch(epoch2).toString(QStringLiteral("yyyy-MM-dd"));

    // Blame lines 2, 5, 9 in one invocation: 2 and 9 are untouched since
    // commit 1 (sha1); 5 was rewritten in commit 2 (sha2).
    QByteArray blameOut;
    ASSERT_TRUE(runGit(dir, {QStringLiteral("blame"), QStringLiteral("--line-porcelain"),
                             QStringLiteral("-L"), QStringLiteral("2,2"),
                             QStringLiteral("-L"), QStringLiteral("5,5"),
                             QStringLiteral("-L"), QStringLiteral("9,9"),
                             QStringLiteral("HEAD"), QStringLiteral("--"),
                             QStringLiteral("blamed.txt")}, {}, &blameOut))
        << "git blame --line-porcelain failed";
    ASSERT_FALSE(blameOut.isEmpty()) << "test setup: blame produced no output";

    const QHash<int, GitBlame::Entry> parsed = GitBlame::parseLinePorcelain(blameOut);

    EXPECT_EQ(parsed.size(), 3)
        << "ANTS-5040 INV-1: expected exactly 3 entries (lines 2, 5, 9); got "
        << parsed.size() << ". parseLinePorcelain is still the stub "
        << "(returns {} unconditionally) until the fix lands.";

    ASSERT_TRUE(parsed.contains(2)) << "no entry for line 2";
    EXPECT_EQ(parsed.value(2).sha, sha1)
        << "line 2 sha: expected " << sha1.toStdString() << " (commit 1), got "
        << parsed.value(2).sha.toStdString();
    EXPECT_EQ(parsed.value(2).author, QStringLiteral("Blame Tester"))
        << "line 2 author: got \"" << parsed.value(2).author.toStdString() << "\"";
    EXPECT_EQ(parsed.value(2).date, expectedDate1)
        << "line 2 date: expected " << expectedDate1.toStdString() << ", got "
        << parsed.value(2).date.toStdString();

    ASSERT_TRUE(parsed.contains(9)) << "no entry for line 9";
    EXPECT_EQ(parsed.value(9).sha, sha1)
        << "line 9 sha: expected " << sha1.toStdString() << " (commit 1), got "
        << parsed.value(9).sha.toStdString();
    EXPECT_EQ(parsed.value(9).date, expectedDate1)
        << "line 9 date: expected " << expectedDate1.toStdString() << ", got "
        << parsed.value(9).date.toStdString();

    ASSERT_TRUE(parsed.contains(5)) << "no entry for line 5";
    EXPECT_EQ(parsed.value(5).sha, sha2)
        << "line 5 sha: expected " << sha2.toStdString() << " (commit 2, the "
        << "rewritten line — this is the case that exercises --line-porcelain's "
        << "per-line header repetition), got " << parsed.value(5).sha.toStdString();
    EXPECT_EQ(parsed.value(5).author, QStringLiteral("Blame Tester"))
        << "line 5 author: got \"" << parsed.value(5).author.toStdString() << "\"";
    EXPECT_EQ(parsed.value(5).date, expectedDate2)
        << "line 5 date: expected " << expectedDate2.toStdString() << ", got "
        << parsed.value(5).date.toStdString();
}

// ---------------------------------------------------------------------
// INV-2 — the wiring. Source-scrape, comments stripped.
// ---------------------------------------------------------------------

TEST(AuditBlameBulkAsync, NoSynchronousWaitInTheBlamePath) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    std::string detail;
    bool blocks = false;

    // enrichWithBlame() may not survive the refactor (the fix note folds
    // batching into a different shape) — tolerate its absence, but if it's
    // still there, it must not wait synchronously.
    const std::string enrichBody =
        ants_test::slurpFunctionBody(stripped, "void AuditDialog::enrichWithBlame(");
    if (!enrichBody.empty() &&
        enrichBody.find("waitForFinished") != std::string::npos) {
        blocks = true;
        detail += "AuditDialog::enrichWithBlame() still calls waitForFinished "
                  "on the GUI thread. ";
    }

    // renderResults() itself must always exist (it's the entry point named
    // in the roadmap item) — anchor on the qualified definition so a bare
    // "renderResults(" match in the header declaration or a comment can't
    // be picked up first.
    const std::string renderBody =
        ants_test::slurpFunctionBody(stripped, "void AuditDialog::renderResults(");
    ASSERT_FALSE(renderBody.empty())
        << "AuditDialog::renderResults() body not found — anchor moved?";
    if (renderBody.find("waitForFinished") != std::string::npos) {
        blocks = true;
        detail += "AuditDialog::renderResults() itself calls waitForFinished. ";
    }

    EXPECT_FALSE(blocks)
        << "ANTS-5040 INV-2: the blame path reachable from renderResults() "
           "must not block the GUI thread on git blame. " << detail;
}

TEST(AuditBlameBulkAsync, ArgvUsesLinePorcelainFlag) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    EXPECT_NE(stripped.find("--line-porcelain"), std::string::npos)
        << "ANTS-5040 INV-2: no \"--line-porcelain\" invocation found in "
           "auditdialog.cpp — expected: the batched blame call uses "
           "--line-porcelain (each requested line needs its own full commit "
           "header, since several -L ranges can land on different commits); "
           "actual: only the single-line --porcelain form is present, if any.";
}

TEST(AuditBlameBulkAsync, ArgvBuildsMultipleRangesPerProcess) {
    const std::string stripped =
        ants_test::stripComments(ants_test::slurpFile(SRC_AUDIT_CPP_PATH));
    ASSERT_FALSE(stripped.empty());

    const size_t pcPos = stripped.find("--line-porcelain");
    ASSERT_NE(pcPos, std::string::npos)
        << "ANTS-5040 INV-2: cannot verify multi-range batching without a "
           "--line-porcelain invocation to anchor on (see "
           "ArgvUsesLinePorcelainFlag).";

    // Bounded window around the flag, the same house pattern
    // audit_tool_process_group_kill's group-kill scrape uses for "nearby in
    // the same construction", rather than parsing the enclosing function by
    // name (which the fix is free to rename).
    constexpr size_t kWindow = 500;
    const size_t begin = pcPos > kWindow ? pcPos - kWindow : 0;
    const size_t end = std::min(stripped.size(), pcPos + kWindow);
    const std::string region = stripped.substr(begin, end - begin);

    const bool hasRangeFlag = region.find("\"-L\"") != std::string::npos;
    ASSERT_TRUE(hasRangeFlag)
        << "ANTS-5040 INV-2: no \"-L\" argument found near the "
           "--line-porcelain invocation — expected: the blame argv still "
           "carries per-line -L ranges; actual context: \"" << region << "\"";

    // Multiple ranges per process is satisfied either by an unrolled argv
    // (the "-L" literal written out more than once) or by a single "-L"
    // literal built inside a visible loop (the loop runs once per line at
    // RUNTIME, so the SOURCE only needs the literal once) — both are
    // correct implementations of "several -L ranges in one process", and
    // neither should be required over the other.
    const bool unrolled = ants_test::countOccurrences(region, "\"-L\"") >= 2;
    const bool loopBuilt =
        region.find("for (") != std::string::npos ||
        region.find("for(") != std::string::npos ||
        region.find("while (") != std::string::npos ||
        region.find("while(") != std::string::npos;

    EXPECT_TRUE(unrolled || loopBuilt)
        << "ANTS-5040 INV-2: the blame argv near --line-porcelain shows no "
           "evidence of more than one -L range per process — expected: "
           "either \"-L\" written more than once, or a for/while loop "
           "building it (one process per FILE, several ranges); actual: "
           "exactly one \"-L\" literal and no loop keyword in the "
           "surrounding " << (2 * kWindow) << "-byte window: \"" << region
        << "\"";
}
