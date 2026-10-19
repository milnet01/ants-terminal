// Feature-conformance test for spec.md (ANTS-5774) — rendering the
// Review Changes diff must not sever connections other code made to the
// Copy Diff button. Qt's style-sheet cache cleanup is one such
// connection; with it cut, the deleted button stayed in the cache and
// the next qApp->setStyleSheet() crashed.

#include "diffviewer.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextStream>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

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

bool writeTextFile(const QString &path, const QString &content) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream(&f) << content;
    return true;
}

// A one-file repo whose working copy differs from HEAD. A LOCAL identity,
// so the fixture never depends on or touches the host's git config.
bool makeDirtyRepo(const QString &dir) {
    const QString file = dir + QStringLiteral("/f.txt");
    return runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                        QStringLiteral("Test")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                        QStringLiteral("test@example.com")})
        && writeTextFile(file, QStringLiteral("v1\n"))
        && runGit(dir, {QStringLiteral("add"), QStringLiteral("f.txt")})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("initial")})
        && writeTextFile(file, QStringLiteral("v2\n"));
}

QPushButton *copyButton(QDialog *dialog) {
    for (QPushButton *b : dialog->findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Copy Diff")) return b;
    return nullptr;
}

}  // namespace

// INV-1
TEST(ReviewChangesStyleCache, RenderKeepsOthersLinksToCopyButton) {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
        GTEST_SKIP() << "git not in PATH";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(makeDirtyRepo(tmp.path())) << "git fixture setup failed";

    QWidget host;
    QDialog *dialog = diffviewer::show(&host, tmp.path(), QStringLiteral("Dark"));
    ASSERT_NE(dialog, nullptr);
    auto *viewer = dialog->findChild<QTextBrowser *>();
    ASSERT_NE(viewer, nullptr);
    QPushButton *copy = copyButton(dialog);
    ASSERT_NE(copy, nullptr) << "no \"Copy Diff\" button in the dialog";

    // Stands in for Qt's style-cache cleanup, which rides the same signal.
    bool probeFired = false;
    QObject::connect(copy, &QObject::destroyed, [&probeFired] { probeFired = true; });

    QElapsedTimer t;
    t.start();
    while (viewer->toPlainText().contains(QStringLiteral("Loading git status"))
           && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    ASSERT_FALSE(viewer->toPlainText().contains(QStringLiteral("Loading git status")))
        << "diff viewer never rendered within 30s";

    dialog->close();  // WA_DeleteOnClose
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    EXPECT_TRUE(probeFired)
        << "rendering the diff severed a connection other code made to the "
           "Copy Diff button; Qt's style-sheet cache cleanup rides the same "
           "destroyed() signal, so the deleted button stays in the cache and "
           "the next qApp->setStyleSheet() crashes (ANTS-5774)";
}
