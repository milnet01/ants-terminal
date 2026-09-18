// The roadmap dialog's rebuild reads only what moved, and says when it cannot
// read — see spec.md. ANTS-5088.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/xdg_guard.h"
#include "support/testspawn.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QTextBrowser>

#include <gtest/gtest.h>

namespace {

const char *kRoadmap =
    "# ROADMAP\n"
    "\n"
    "## Now\n"
    "\n"
    "- 📋 [ANTS-0001] **A planned thing.**\n"
    "  Kind: fix.\n";

// Both XDG roots go to the temp dir: the dialog opens the roadmap store, and
// the store's default path would otherwise be the machine's real one.
struct Harness {
    ants_test::XdgGuard guard;
    QTemporaryDir       dir;
    QString             path;

    Harness() {
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toUtf8());
        guard.setEnv("XDG_DATA_HOME", dir.filePath(QStringLiteral("data")).toUtf8());
        path = dir.filePath(QStringLiteral("ROADMAP.md"));
    }
    bool write(const QString &file, const QByteArray &bytes) const {
        QFile f(dir.filePath(file));
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
    }
};

template <typename Pred>
bool spinUntil(Pred p, int budgetMs = 30000) {
    QElapsedTimer t;
    t.start();
    while (!p() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return p();
}

// Let construction's own work settle: the first rebuild, the git runs it
// starts, and the debounced re-render their answers schedule.
void settle(RoadmapDialog &dlg) {
    spinUntil([&] {
        return !dlg.recentCommitsInFlight() && !dlg.lastTouchBlameInFlight();
    });
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 400)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

void rebuild(RoadmapDialog &dlg) {
    ASSERT_TRUE(QMetaObject::invokeMethod(&dlg, "rebuild", Qt::DirectConnection));
}

bool gitAvailable() {
    QProcess p;
    p.start(QStringLiteral("git"), {QStringLiteral("--version")});
    return ants_test::waitForHelper(p) && p.exitCode() == 0;
}

bool git(const QString &dir, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    return p.waitForStarted(2000) && p.waitForFinished(15000) && p.exitCode() == 0;
}

}  // namespace

// INV-1
TEST(RoadmapDialogRebuildCost, OversizedLiveRoadmapIsReported) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ROADMAP.md"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("# Roadmap\n");
        ASSERT_TRUE(f.resize(qint64{64} * 1024 * 1024 + 1));   // sparse
    }
    QString err;
    const QString md = RoadmapDialog::loadMarkdown(path, false, &err);
    EXPECT_TRUE(md.isEmpty()) << "an oversized roadmap was returned cut short";
    EXPECT_TRUE(err.contains(path)) << "no report names the file: " << err.toStdString();
}

// INV-2
TEST(RoadmapDialogRebuildCost, UnreadableLiveRoadmapIsReported) {
    Harness h;
    QString err;
    const QString md = RoadmapDialog::loadMarkdown(h.path, false, &err);
    EXPECT_TRUE(md.isEmpty());
    EXPECT_TRUE(err.contains(h.path)) << "no report names the file";

    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    auto *viewer = dlg.findChild<QTextBrowser *>();
    ASSERT_NE(viewer, nullptr);
    const QString shown = viewer->toPlainText();
    EXPECT_TRUE(shown.contains(QStringLiteral("Could not read this roadmap.")))
        << "a missing roadmap rendered as: " << shown.toStdString();
    EXPECT_TRUE(shown.contains(h.path));
}

// INV-3
TEST(RoadmapDialogRebuildCost, SourceIsReadOncePerChange) {
    Harness h;
    ASSERT_TRUE(h.write(QStringLiteral("ROADMAP.md"), kRoadmap));
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    settle(dlg);

    const int before = dlg.sourceReadsForTest();
    ASSERT_GE(before, 1) << "construction never read the roadmap";
    rebuild(dlg);
    rebuild(dlg);
    EXPECT_EQ(dlg.sourceReadsForTest(), before)
        << "a rebuild with nothing changed re-read the roadmap";

    // A different size moves the stamp whatever the clock's resolution.
    ASSERT_TRUE(h.write(QStringLiteral("ROADMAP.md"),
                        QByteArray(kRoadmap) + "\n## Later\n"));
    rebuild(dlg);
    EXPECT_EQ(dlg.sourceReadsForTest(), before + 1)
        << "a changed roadmap was not re-read";
}

// INV-4
TEST(RoadmapDialogRebuildCost, UndatedChangelogIsParsedOnce) {
    Harness h;
    ASSERT_TRUE(h.write(QStringLiteral("ROADMAP.md"), kRoadmap));
    ASSERT_TRUE(h.write(QStringLiteral("CHANGELOG.md"),
                        "# Changelog\n\n## [Unreleased]\n\n- Something.\n"));
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    settle(dlg);

    const int before = dlg.shippedDateParsesForTest();
    ASSERT_GE(before, 1);
    rebuild(dlg);
    rebuild(dlg);
    EXPECT_EQ(dlg.shippedDateParsesForTest(), before)
        << "a CHANGELOG with no dated release was re-parsed per rebuild";
}

// INV-5
TEST(RoadmapDialogRebuildCost, RecentCommitsDoNotBlock) {
    if (!gitAvailable()) GTEST_SKIP() << "git not on PATH";
    Harness h;
    ASSERT_TRUE(h.write(QStringLiteral("ROADMAP.md"), kRoadmap));
    const QString d = h.dir.path();
    ASSERT_TRUE(git(d, {QStringLiteral("init"), QStringLiteral("-q")}));
    ASSERT_TRUE(git(d, {QStringLiteral("config"), QStringLiteral("user.email"),
                        QStringLiteral("t@example.invalid")}));
    ASSERT_TRUE(git(d, {QStringLiteral("config"), QStringLiteral("user.name"),
                        QStringLiteral("Test")}));
    ASSERT_TRUE(git(d, {QStringLiteral("config"), QStringLiteral("commit.gpgsign"),
                        QStringLiteral("false")}));
    ASSERT_TRUE(git(d, {QStringLiteral("add"), QStringLiteral("ROADMAP.md")}));
    ASSERT_TRUE(git(d, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("fixture subject")}));

    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    // Structural, not a stopwatch: a synchronous `git log` would have the
    // subject in hand by the time the constructor's rebuild returned.
    EXPECT_TRUE(dlg.recentCommitSubjects().isEmpty())
        << "construction returned with the git log answer already in hand";
    EXPECT_TRUE(dlg.recentCommitsInFlight()) << "no git log was dispatched";

    ASSERT_TRUE(spinUntil([&] { return !dlg.recentCommitSubjects().isEmpty(); }))
        << "the git log never produced a subject";
    EXPECT_TRUE(dlg.recentCommitSubjects().contains(QStringLiteral("fixture subject")));
}

// INV-6
TEST(RoadmapDialogRebuildCost, CurrentWorkTintFollowsToolUse) {
    QFile src(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
              + QStringLiteral("/../../../src/roadmapdialog.cpp"));
    ASSERT_TRUE(src.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_FALSE(QString::fromUtf8(src.readAll())
                     .contains(QStringLiteral("rgba(229,194,74")))
        << "the tint repeats the ToolUse colour as a literal";

    const QString html = RoadmapDialog::renderCardsHtml(
        QString::fromUtf8(kRoadmap), RoadmapDialog::ShowPlanned, {},
        QStringLiteral("light"));
    EXPECT_TRUE(html.contains(QStringLiteral(".rm-cur{background:rgba(229,194,74,0.08);}")))
        << "the rendered tint changed";
}
