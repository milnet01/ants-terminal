// Feature-conformance test for spec.md (ANTS-6620) — the Review Changes
// dialog reads git's quoted status paths, and a new probe round cancels the
// one still running.
//
//   INV-1 — a quoted or octal-escaped status path is read as the real path.
//   INV-2 — Refresh cancels the round in flight instead of adding to it.

#include "diffviewer.h"

#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

bool gitOnPath() {
    return !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
}

bool runGit(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(5000)) return false;
    if (!p.waitForFinished(60000)) {
        p.kill();
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           f.write(bytes) == bytes.size();
}

// git init + a LOCAL identity (never the host's config) + one commit of
// `relPath`.
bool makeRepoAndCommit(const QString &dir, const QString &relPath,
                       const QByteArray &content) {
    return runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                        QStringLiteral("Test")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                        QStringLiteral("test@example.com")})
        && writeBytes(dir + QLatin1Char('/') + relPath, content)
        && runGit(dir, {QStringLiteral("add"), relPath})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("initial")});
}

// The dialog shows a "Loading git status" placeholder until every probe of
// the first round has finished and finalize() has rendered.
QString renderOnce(const QString &dir) {
    QWidget host;
    QDialog *dialog = diffviewer::show(&host, dir, QStringLiteral("Dark"));
    if (!dialog) return {};
    auto *viewer = dialog->findChild<QTextBrowser *>();
    QString text;
    if (viewer) {
        QElapsedTimer t;
        t.start();
        while (viewer->toPlainText().contains(QStringLiteral("Loading git status"))
               && t.elapsed() < 60000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        text = viewer->toPlainText();
    }
    dialog->close();
    QCoreApplication::processEvents();
    return text;
}

}  // namespace

// INV-1 — git quotes a path holding a space and, by default, escapes a
// non-ASCII one as octal bytes. Read raw, `"a b.txt"` named no file.
TEST(ReviewChangesProbeRounds, Ants6620QuotedStatusPathsResolve) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeRepoAndCommit(tmp.path(), QStringLiteral("c d.txt"),
                                  "one\n"));
    ASSERT_TRUE(writeBytes(tmp.filePath(QStringLiteral("c d.txt")),
                           "one\ntwo\nthree\n"));
    ASSERT_TRUE(writeBytes(tmp.filePath(QStringLiteral("a b.txt")),
                           "spaced-content\n"));
    ASSERT_TRUE(writeBytes(tmp.filePath(QString::fromUtf8("\xc3\xa9.txt")),
                           "accent-content\n"));

    const QString text = renderOnce(tmp.path());
    ASSERT_FALSE(text.isEmpty()) << "the dialog never rendered";
    EXPECT_TRUE(text.contains(QStringLiteral("+spaced-content")))
        << "an untracked file with a space in its name must show its content";
    EXPECT_TRUE(text.contains(QStringLiteral("+accent-content")))
        << "an untracked file with a non-ASCII name must show its content";
    EXPECT_FALSE(text.contains(QStringLiteral("(unreadable)")))
        << "no New-files entry may read as unreadable; got:\n" << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("(3 lines)")))
        << "a modified file with a space in its name must get its line count";
}

// INV-2 — each refresh used to start five more git processes beside the
// ones still running, so a change burst longer than a probe piled them up.
TEST(ReviewChangesProbeRounds, Ants6620RefreshCancelsTheRunningRound) {
    if (!gitOnPath()) GTEST_SKIP() << "git not in PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeRepoAndCommit(tmp.path(), QStringLiteral("f.txt"), "v1\n"));

    QWidget host;
    QDialog *dialog = diffviewer::show(&host, tmp.path(), QStringLiteral("Dark"));
    ASSERT_NE(dialog, nullptr);
    auto *refresh = dialog->findChild<QPushButton *>(QStringLiteral("reviewRefreshBtn"));
    ASSERT_NE(refresh, nullptr) << "no Refresh button (objectName reviewRefreshBtn)";

    // No event processing in between: a probe's exit is only noticed by the
    // event loop, so every round's processes still count as running unless
    // the dialog itself stopped them.
    refresh->click();
    refresh->click();
    int runningStatus = 0;
    for (QProcess *p : dialog->findChildren<QProcess *>())
        if (p->arguments().contains(QStringLiteral("status")) &&
            p->state() != QProcess::NotRunning)
            ++runningStatus;
    EXPECT_EQ(runningStatus, 1)
        << "after the open and two refreshes, only the newest round's "
           "git status may still run";

    dialog->close();
    QCoreApplication::processEvents();
}
