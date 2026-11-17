// Feature-conformance test for spec.md (ANTS-6806) — Review Changes names
// the uncommitted work in a "Work in progress" section above Status.

#include "diffviewer.h"
#include "reviewwip.h"

#include <QCoreApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QWidget>

#include <gtest/gtest.h>

namespace {

const QHash<QString, QString> kHeadlines = {
    {QStringLiteral("DEMO-1"), QStringLiteral("Roadmap headline one")},
    {QStringLiteral("DEMO-2"), QStringLiteral("Roadmap headline two")},
    {QStringLiteral("DEMO-3"), QStringLiteral("Roadmap headline three")},
};

// A `git diff --patch HEAD` with one file per (path, body) pair.
QString diffOf(const QList<QPair<QString, QString>> &files) {
    QString out;
    for (const auto &[path, body] : files) {
        out += QStringLiteral("diff --git a/%1 b/%1\n"
                              "index 1111111..2222222 100644\n"
                              "--- a/%1\n+++ b/%1\n@@ -1,2 +1,3 @@\n")
                   .arg(path);
        out += body;
    }
    return out;
}

// ---- INV-1 -------------------------------------------------------------
TEST(ReviewChangesWip, ChangelogBulletNamesTheId) {
    const QString diff = diffOf({
        {QStringLiteral("CHANGELOG.md"),
         QStringLiteral(" ### Added\n"
                        "+- **The dialog names the work in progress.** (DEMO-1)\n"
                        "+  Continuation prose.\n")},
        {QStringLiteral("src/a.cpp"),
         QStringLiteral("+// DEMO-1 — the new code.\n")},
    });
    EXPECT_EQ(reviewwip::lines(diff, kHeadlines),
              QStringList{QStringLiteral(
                  "DEMO-1: The dialog names the work in progress")});
}

// ---- INV-1, a bullet with no bold span ---------------------------------
TEST(ReviewChangesWip, PlainChangelogBulletDropsTheCitation) {
    const QString diff = diffOf({
        {QStringLiteral("CHANGELOG.md"),
         QStringLiteral("+- Plain bullet text. (DEMO-2)\n")},
    });
    EXPECT_EQ(reviewwip::lines(diff, kHeadlines),
              QStringList{QStringLiteral("DEMO-2: Plain bullet text")});
}

// ---- INV-2 -------------------------------------------------------------
TEST(ReviewChangesWip, NoBulletFallsBackToTheHeadline) {
    const QString diff = diffOf({
        {QStringLiteral("src/a.cpp"),
         QStringLiteral(" context\n+// DEMO-2 — a new guard.\n")},
    });
    EXPECT_EQ(reviewwip::lines(diff, kHeadlines),
              QStringList{QStringLiteral("DEMO-2: Roadmap headline two")});
}

// ---- INV-3 -------------------------------------------------------------
TEST(ReviewChangesWip, OnlyRoadmapIdsCount) {
    const QString diff = diffOf({
        {QStringLiteral("src/a.cpp"),
         QStringLiteral("+// UTF-8 and DEMO-99 and XDEMO-1 are not roadmap ids.\n")},
    });
    EXPECT_TRUE(reviewwip::lines(diff, kHeadlines).isEmpty())
        << reviewwip::lines(diff, kHeadlines).join(QLatin1Char('|')).toStdString();
}

// ---- INV-4 -------------------------------------------------------------
TEST(ReviewChangesWip, RemovedAndContextLinesDoNotCount) {
    const QString diff = diffOf({
        {QStringLiteral("src/a.cpp"),
         QStringLiteral(" // DEMO-1 context\n-// DEMO-2 removed\n+plain added\n")},
    });
    EXPECT_TRUE(reviewwip::lines(diff, kHeadlines).isEmpty())
        << reviewwip::lines(diff, kHeadlines).join(QLatin1Char('|')).toStdString();
}

// ---- INV-5 -------------------------------------------------------------
TEST(ReviewChangesWip, EachIdOnceInFirstMentionOrder) {
    const QString diff = diffOf({
        {QStringLiteral("src/a.cpp"),
         QStringLiteral("+// DEMO-3 then DEMO-1\n+// DEMO-3 again\n")},
    });
    EXPECT_EQ(reviewwip::lines(diff, kHeadlines),
              (QStringList{QStringLiteral("DEMO-3: Roadmap headline three"),
                           QStringLiteral("DEMO-1: Roadmap headline one")}));
}

// ---- The dialog --------------------------------------------------------

bool runGit(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(5000) || !p.waitForFinished(60000)) return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool writeFile(const QString &path, const QString &text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(text.toUtf8()) >= 0;
}

const QString kRoadmap = QStringLiteral(
    "# Roadmap\n\n## Work\n\n"
    "- 📋 [DEMO-1] **Roadmap headline one.**\n"
    "  Kind: feature.\n\n"
    "- 📋 [DEMO-2] **Roadmap headline two.**\n"
    "  Kind: fix.\n");

// A repository holding ROADMAP.md, CHANGELOG.md and src.txt, committed.
bool makeRepo(const QString &dir) {
    return runGit(dir, {QStringLiteral("init"), QStringLiteral("-q")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.name"),
                        QStringLiteral("Test")})
        && runGit(dir, {QStringLiteral("config"), QStringLiteral("user.email"),
                        QStringLiteral("test@example.com")})
        && writeFile(dir + QStringLiteral("/ROADMAP.md"), kRoadmap)
        && writeFile(dir + QStringLiteral("/CHANGELOG.md"),
                     QStringLiteral("# Changelog\n\n## [Unreleased]\n\n### Added\n\n"))
        && writeFile(dir + QStringLiteral("/src.txt"), QStringLiteral("one\n"))
        && runGit(dir, {QStringLiteral("add"), QStringLiteral(".")})
        && runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("initial")});
}

// The dialog's text once its probes have rendered.
QString renderedText(const QString &dir, const QString &roadmapPath) {
    QWidget host;
    QDialog *dialog = diffviewer::show(&host, dir, QStringLiteral("Dark"),
                                       roadmapPath);
    auto *viewer = dialog->findChild<QTextBrowser *>();
    if (!viewer) return QStringLiteral("<no viewer>");
    QElapsedTimer t;
    t.start();
    while (viewer->toPlainText().contains(QStringLiteral("Loading git status"))
           && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const QString text = viewer->toPlainText();
    dialog->close();
    QCoreApplication::processEvents();
    return text;
}

// ---- INV-7 -------------------------------------------------------------
TEST(ReviewChangesWip, HeadlinesReadsTheRoadmapFile) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(writeFile(path, kRoadmap));
    const QHash<QString, QString> h = reviewwip::headlines(path);
    EXPECT_EQ(h.value(QStringLiteral("DEMO-1")),
              QStringLiteral("Roadmap headline one"));
    EXPECT_EQ(h.value(QStringLiteral("DEMO-2")),
              QStringLiteral("Roadmap headline two"));
    EXPECT_TRUE(reviewwip::headlines(tmp.path() + QStringLiteral("/none.md"))
                    .isEmpty());
}

// ---- INV-6 -------------------------------------------------------------
TEST(ReviewChangesWip, DialogShowsTheSectionAboveStatus) {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
        GTEST_SKIP() << "git not in PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_TRUE(makeRepo(dir)) << "git fixture setup failed";
    ASSERT_TRUE(writeFile(dir + QStringLiteral("/CHANGELOG.md"),
        QStringLiteral("# Changelog\n\n## [Unreleased]\n\n### Added\n\n"
                       "- **The section names the work.** (DEMO-1)\n")));
    ASSERT_TRUE(writeFile(dir + QStringLiteral("/src.txt"),
                          QStringLiteral("one\nDEMO-2 guard\n")));

    const QString text = renderedText(dir, dir + QStringLiteral("/ROADMAP.md"));
    const qsizetype wip = text.indexOf(QStringLiteral("━━ Work in progress ━━"));
    const qsizetype status = text.indexOf(QStringLiteral("━━ Status ━━"));
    ASSERT_GE(wip, 0) << "no Work in progress heading in:\n" << text.toStdString();
    ASSERT_GE(status, 0) << "no Status heading in:\n" << text.toStdString();
    EXPECT_LT(wip, status) << "Work in progress is not above Status";
    const QString between = text.mid(wip, status - wip);
    EXPECT_TRUE(between.contains(
        QStringLiteral("DEMO-1: The section names the work")))
        << between.toStdString();
    EXPECT_TRUE(between.contains(QStringLiteral("DEMO-2: Roadmap headline two")))
        << between.toStdString();
}

TEST(ReviewChangesWip, NoRoadmapIdNoSection) {
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
        GTEST_SKIP() << "git not in PATH";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_TRUE(makeRepo(dir)) << "git fixture setup failed";
    ASSERT_TRUE(writeFile(dir + QStringLiteral("/src.txt"),
                          QStringLiteral("one\ntwo\n")));

    const QString text = renderedText(dir, dir + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(text.contains(QStringLiteral("━━ Status ━━"))) << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("Work in progress")))
        << text.toStdString();
}

}  // namespace
