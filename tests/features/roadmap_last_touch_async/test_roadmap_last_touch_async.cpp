// ANTS-4414 — the last-touch blame no longer holds the GUI thread.
// Contract: spec.md beside this file.
//
// INV-1 is asserted structurally rather than with a stopwatch. "Returned in
// under N ms" is a race against whatever else the machine is doing; "the answer
// is not there yet, and is there after the event loop runs" cannot pass against
// a synchronous implementation and cannot fail on a slow box.

#include "config.h"
#include "roadmapdialog.h"

#include "../../_support/xdg_guard.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include "support/testspawn.h"

#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

// One 🚧 bullet with a two-line body, one 📋 that must NOT be dated, and a
// trailing blank line so the block walk has a terminator to find.
const char *kFixture =
    "# ROADMAP\n"
    "\n"
    "## Now\n"
    "\n"
    "- 🚧 [ANTS-0001] **An in-progress thing.**\n"
    "  Kind: implement.\n"
    "  Source: fixture.\n"
    "\n"
    "- 📋 [ANTS-0002] **A planned thing.**\n"
    "  Kind: fix.\n";

bool gitAvailable() {
    QProcess p;
    p.start(QStringLiteral("git"), {QStringLiteral("--version")});
    return ants_test::waitForHelper(p) && p.exitCode() == 0;   // ANTS-4651
}

// A committed repo. Identity is set LOCALLY and signing disabled: the machine's
// global git config is not a prerequisite for this suite, and a developer with
// commit.gpgsign=true would otherwise get a hang instead of a test result.
bool makeRepo(const QString &dir, const QString &roadmapPath) {
    QFile f(roadmapPath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(kFixture);
    f.close();

    const QVector<QStringList> steps = {
        {QStringLiteral("init"), QStringLiteral("-q")},
        {QStringLiteral("config"), QStringLiteral("user.email"),
         QStringLiteral("t@example.invalid")},
        {QStringLiteral("config"), QStringLiteral("user.name"),
         QStringLiteral("Test")},
        {QStringLiteral("config"), QStringLiteral("commit.gpgsign"),
         QStringLiteral("false")},
        {QStringLiteral("add"), QStringLiteral("ROADMAP.md")},
        {QStringLiteral("commit"), QStringLiteral("-q"),
         QStringLiteral("-m"), QStringLiteral("fixture")},
    };
    for (const QStringList &args : steps) {
        QProcess p;
        p.setWorkingDirectory(dir);
        p.start(QStringLiteral("git"), args);
        if (!p.waitForStarted(2000) || !p.waitForFinished(15000)
            || p.exitCode() != 0)
            return false;
    }
    return true;
}

struct Harness {
    ants_test::XdgGuard guard;
    QTemporaryDir       dir;
    QString             path;

    Harness() {
        // See tests/features/roadmap_toc_toggle/spec.md — test mode makes
        // QStandardPaths ignore XDG_CONFIG_HOME and a sibling's Config leaks in.
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.path().toUtf8());
        path = dir.filePath(QStringLiteral("ROADMAP.md"));
    }
};

// Spin until the predicate holds or the budget expires. Generous budget: this
// is guarding against "never", not measuring latency.
template <typename Pred>
bool spinUntil(Pred p, int budgetMs = 30000) {
    QElapsedTimer t;
    t.start();
    while (!p() && t.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return p();
}

}  // namespace

// INV-4 — the parser, with no git anywhere near it.
TEST(RoadmapLastTouchAsync, ParserTakesMaxOverTheBulletBlock) {
    Harness h;
    QFile f(h.path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(kFixture);
    f.close();

    // --line-porcelain: "<40-hex> <orig> <final>", then header lines, then the
    // content line prefixed with TAB. Line 5 is the 🚧 bullet, 6 and 7 its body.
    const auto rec = [](int line, qint64 when, const char *text) {
        return QStringLiteral("%1 %2 %2\nauthor-time %3\n\t%4\n")
            .arg(QString(40, QLatin1Char('a')))
            .arg(line)
            .arg(when)
            .arg(QLatin1String(text));
    };
    QString blame;
    blame += rec(1, 1000, "# ROADMAP");
    blame += rec(2, 1000, "");
    blame += rec(3, 1000, "## Now");
    blame += rec(4, 1000, "");
    blame += rec(5, 1500, "- 🚧 [ANTS-0001] **An in-progress thing.**");
    blame += rec(6, 9999, "  Kind: implement.");   // the MAX, mid-block
    blame += rec(7, 2000, "  Source: fixture.");
    blame += rec(8, 1000, "");
    blame += rec(9, 7777, "- 📋 [ANTS-0002] **A planned thing.**");
    blame += rec(10, 7777, "  Kind: fix.");

    const auto out =
        RoadmapDialog::lastTouchFromBlame(blame.toUtf8(), h.path);

    ASSERT_TRUE(out.contains(QStringLiteral("ANTS-0001")))
        << "the in-progress bullet got no date at all";
    EXPECT_EQ(out.value(QStringLiteral("ANTS-0001")), 9999)
        << "must be the MAX over the block, not the bullet line's own time";
    EXPECT_FALSE(out.contains(QStringLiteral("ANTS-0002")))
        << "only 🚧 bullets carry a last-touch date; a 📋 must not be dated";
}

// INV-1 + INV-2 + INV-5 — the whole point of the change.
TEST(RoadmapLastTouchAsync, RefreshReturnsBeforeTheBlameFinishes) {
    if (!gitAvailable()) GTEST_SKIP() << "git not on PATH";
    Harness h;
    ASSERT_TRUE(makeRepo(h.dir.path(), h.path)) << "fixture repo did not build";

    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);

    // The dialog's own construction runs a rebuild, which may already have
    // started a blame. Let anything in flight settle, then clear and re-arm so
    // this test observes a run it owns.
    ASSERT_TRUE(spinUntil([&] { return !dlg.lastTouchBlameInFlight(); }))
        << "a blame started at construction never finished";

    dlg.resetLastTouchForTest();
    ASSERT_TRUE(dlg.lastTouchDates().isEmpty());

    dlg.refreshLastTouchDatesIfStale();

    // INV-1 — this is the assertion that fails against the old synchronous
    // code, where the hash was fully populated by the time the call returned.
    EXPECT_TRUE(dlg.lastTouchDates().isEmpty())
        << "refreshLastTouchDatesIfStale() returned with the answer already in "
           "hand — it is still blocking on git blame";
    EXPECT_TRUE(dlg.lastTouchBlameInFlight())
        << "no blame is running, so nothing was dispatched";

    // INV-5 — a second call while one is in flight must not start another.
    dlg.refreshLastTouchDatesIfStale();
    EXPECT_TRUE(dlg.lastTouchBlameInFlight());

    // INV-2 — and the answer does arrive.
    ASSERT_TRUE(spinUntil([&] { return !dlg.lastTouchDates().isEmpty(); }))
        << "the blame never produced a date";
    EXPECT_TRUE(dlg.lastTouchDates().contains(QStringLiteral("ANTS-0001")));
    EXPECT_FALSE(dlg.lastTouchBlameInFlight())
        << "the process handle outlived the run";
}

// INV-6 — an empty answer is still an answer.
TEST(RoadmapLastTouchAsync, EmptyResultIsNotRetriedForever) {
    Harness h;
    // Deliberately NOT a git repo: the blame fails, the hash stays empty, and a
    // guard keyed on the hash being non-empty would re-dispatch on every call.
    QFile f(h.path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(kFixture);
    f.close();

    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    ASSERT_TRUE(spinUntil([&] { return !dlg.lastTouchBlameInFlight(); }));

    dlg.resetLastTouchForTest();
    dlg.refreshLastTouchDatesIfStale();
    ASSERT_TRUE(spinUntil([&] { return !dlg.lastTouchBlameInFlight(); }))
        << "the failing run never completed";
    ASSERT_TRUE(dlg.lastTouchDates().isEmpty()) << "not a repo — expect empty";

    // The file has not changed, so the completed run stands and nothing new
    // may be dispatched.
    dlg.refreshLastTouchDatesIfStale();
    EXPECT_FALSE(dlg.lastTouchBlameInFlight())
        << "an empty result was treated as 'never ran' and re-dispatched — on "
           "a real roadmap that is one git blame per keystroke";
}

// INV-7 — the synchronous form survives for ANTS-1237's tests.
TEST(RoadmapLastTouchAsync, SynchronousFormStillWorks) {
    if (!gitAvailable()) GTEST_SKIP() << "git not on PATH";
    Harness h;
    ASSERT_TRUE(makeRepo(h.dir.path(), h.path));

    const auto out = RoadmapDialog::parseLastTouchDates(h.path);
    EXPECT_TRUE(out.contains(QStringLiteral("ANTS-0001")))
        << "parseLastTouchDates() stopped returning the in-progress date";
}
