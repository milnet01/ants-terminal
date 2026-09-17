// ANTS-5084 — the audit dialog reads git history off the GUI thread.
// Contract: spec.md beside this file.

#include "auditdialog.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QTemporaryDir>

namespace {

bool haveGit() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

// ANTS-3841 — never inherit a git environment that points at the real repo.
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
    return p.waitForFinished(30000) && p.exitStatus() == QProcess::NormalExit
        && p.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(body) == body.size();
}

bool commitAll(const QString &dir, const char *message) {
    return runGit(dir, {QStringLiteral("add"), QStringLiteral("-A")})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QString::fromLatin1(message)});
}

// Two commits: a.txt with three lines, then line 2 changed.
bool makeTwoCommitRepo(const QString &dir) {
    if (!runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})) return false;
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@t.test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("Test")});
    runGit(dir, {QStringLiteral("config"), QStringLiteral("commit.gpgsign"), QStringLiteral("false")});
    const QString a = dir + QStringLiteral("/a.txt");
    return writeFile(a, "one\ntwo\nthree\n") && commitAll(dir, "seed")
        && writeFile(a, "one\nTWO\nthree\n") && commitAll(dir, "edit");
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int brace = src.indexOf(QChar('{'), start);
    if (brace < 0) return QString();
    int depth = 1;
    int i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src.at(i) == QChar('\'') && i + 2 < src.size() && src.at(i + 2) == QChar('\'')) {
            i += 3;
            continue;
        }
        if (src.at(i) == QChar('{')) ++depth;
        else if (src.at(i) == QChar('}')) --depth;
        ++i;
    }
    return src.mid(brace, i - brace);
}

}  // namespace

// INV-1
TEST(AuditRecentScopeAsync, ChangedFilesAndLines) {
    if (!haveGit()) GTEST_SKIP() << "git not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeTwoCommitRepo(tmp.path()));

    const auto sets = AuditDialog::readRecentChangeSets(tmp.path(), 1, true);
    EXPECT_TRUE(sets.error.isEmpty()) << sets.error.toStdString();
    EXPECT_EQ(sets.files, QStringList{QStringLiteral("a.txt")});
    EXPECT_EQ(sets.lines.value(QStringLiteral("a.txt")), QSet<int>{2});
}

// INV-2
TEST(AuditRecentScopeAsync, ShortHistoryCountsEveryLine) {
    if (!haveGit()) GTEST_SKIP() << "git not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeTwoCommitRepo(tmp.path()));

    const auto sets = AuditDialog::readRecentChangeSets(tmp.path(), 10, true);
    EXPECT_TRUE(sets.error.isEmpty()) << sets.error.toStdString();
    EXPECT_EQ(sets.lines.value(QStringLiteral("a.txt")), (QSet<int>{1, 2, 3}));
}

// INV-3
TEST(AuditRecentScopeAsync, FilesOnlyReadsNoLines) {
    if (!haveGit()) GTEST_SKIP() << "git not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeTwoCommitRepo(tmp.path()));

    const auto sets = AuditDialog::readRecentChangeSets(tmp.path(), 1, false);
    EXPECT_TRUE(sets.error.isEmpty()) << sets.error.toStdString();
    EXPECT_EQ(sets.files, QStringList{QStringLiteral("a.txt")});
    EXPECT_TRUE(sets.lines.isEmpty());
}

// INV-4
TEST(AuditRecentScopeAsync, NotARepositoryReportsAnError) {
    if (!haveGit()) GTEST_SKIP() << "git not installed";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const auto sets = AuditDialog::readRecentChangeSets(tmp.path(), 1, true);
    EXPECT_FALSE(sets.error.isEmpty()) << "git failing outside a repository went unreported";
}

// INV-5
TEST(AuditRecentScopeAsync, NoGitOnTheGuiThread) {
    const QString src = QString::fromStdString(ants_test::slurpAuditDialog());
    ASSERT_FALSE(src.isEmpty()) << "AuditDialog sources not readable";
    EXPECT_FALSE(src.contains(QStringLiteral("computeRecentChangeSets(")))
        << "the blocking git reader is still called";

    for (const char *sig : {"void AuditDialog::runAudit()",
                            "void AuditDialog::onSinceBaselineToggled("}) {
        const QString body = functionBody(src, QString::fromLatin1(sig));
        ASSERT_FALSE(body.isEmpty()) << sig << " not found";
        EXPECT_TRUE(body.contains(QStringLiteral("requestRecentChangeSets(")))
            << sig << " does not request the sets asynchronously";
    }

    const QString request = functionBody(src,
        QStringLiteral("void AuditDialog::requestRecentChangeSets("));
    ASSERT_FALSE(request.isEmpty()) << "requestRecentChangeSets not found";
    const int thread = request.indexOf(QStringLiteral("QThread::create("));
    const int read = request.indexOf(QStringLiteral("readRecentChangeSets("));
    ASSERT_GE(thread, 0) << "the sets are not read on a worker thread";
    EXPECT_GT(read, thread) << "readRecentChangeSets is not called inside the worker";
}
