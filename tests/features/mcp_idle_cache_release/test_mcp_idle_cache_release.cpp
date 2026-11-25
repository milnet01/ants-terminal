// ANTS-5208 — an idle shared-lane worker frees RemoteControl's parse caches.
// See tests/features/mcp_idle_cache_release/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

#include "../../_support/srcgrep.h"
#include "claudeintegration.h"
#include "remotecontrol.h"

namespace {

// An ants-v1 roadmap, padded past RemoteControl::kRoadmapMinParseableSize.
QByteArray roadmapDoc() {
    QByteArray b = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        b += "Padding to clear the minimum-parseable-size gate. \n";
    b += "\n## Section One\n\n"
         "- \xF0\x9F\x93\x8B [TEST-0001] **First headline.**\n"
         "  A body.\n"
         "- \xE2\x9C\x85 [TEST-0002] **Second headline.**\n"
         "  Another body.\n";
    return b;
}

QStringList idsOf(const QJsonObject &resp) {
    QStringList ids;
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray())
        ids << v.toObject().value(QStringLiteral("id")).toString();
    return ids;
}

bool waitFor(const std::function<bool()> &done, int ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    return done();
}

}  // namespace

// INV-1 — releaseIdleCaches drops all three caches; the next query answers
// as before.
TEST(McpIdleCacheRelease, Inv1ReleaseDropsEveryCache) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QFile r(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(r.open(QIODevice::WriteOnly));
    r.write(roadmapDoc());
    r.close();
    QFile c(root + QStringLiteral("/CHANGELOG.md"));
    ASSERT_TRUE(c.open(QIODevice::WriteOnly));
    c.write("# Changelog\n\n## [Unreleased]\n\n### Added\n\n- **A thing.** (TEST-1)\n");
    c.close();

    RemoteControl rc(nullptr);
    QJsonObject all{{QStringLiteral("caller_cwd"), root},
                    {QStringLiteral("status"), QStringLiteral("all")}};
    QJsonObject section = all;
    section[QStringLiteral("section")] = QStringLiteral("section-one");
    const QJsonObject before = rc.cmdRoadmapQuery(all).object();
    ASSERT_TRUE(before.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(before).toJson().toStdString();
    rc.cmdRoadmapQuery(section);
    const QJsonObject cl = rc.cmdChangelogQuery(
        QJsonObject{{QStringLiteral("caller_cwd"), root}}).object();
    ASSERT_TRUE(cl.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(cl).toJson().toStdString();
    ASSERT_TRUE(rc.roadmapCacheHeldForTest()) << "the roadmap query cached nothing";
    ASSERT_GT(rc.sectionCacheSizeForTest(), 0) << "the section query cached nothing";
    ASSERT_TRUE(rc.changelogCacheHeldForTest()) << "the changelog query cached nothing";

    rc.releaseIdleCaches();
    EXPECT_FALSE(rc.roadmapCacheHeldForTest()) << "the roadmap cache was kept";
    EXPECT_EQ(rc.sectionCacheSizeForTest(), 0) << "the section cache was kept";
    EXPECT_FALSE(rc.changelogCacheHeldForTest()) << "the changelog cache was kept";

    const QJsonObject after = rc.cmdRoadmapQuery(all).object();
    EXPECT_EQ(idsOf(after), idsOf(before)) << "a query after the release answered differently";
}

// INV-2 — a burst of queued shared-lane jobs runs the hook once, after the
// last, on the worker; a bulk-lane job never runs it.
TEST(McpIdleCacheRelease, Inv2SharedLaneRunsHookOnceWhenItDrains) {
    // Declared before the integration: a job still queued at the end runs
    // inside ~ClaudeIntegration's join and must not touch a destroyed local.
    std::atomic<int> hookRuns{0};
    std::atomic<int> jobsDone{0};
    std::atomic<int> jobsDoneAtHook{-1};
    std::atomic<QThread *> hookThread{nullptr};
    std::atomic<QThread *> jobThread{nullptr};
    std::atomic<bool> bulkDone{false};
    ClaudeIntegration ci;
    ci.setSharedLaneIdleHook([&] {
        hookThread.store(QThread::currentThread());
        jobsDoneAtHook.store(jobsDone.load());
        ++hookRuns;
    });

    // The first job holds the lane, so the next two queue behind it.
    ASSERT_TRUE(ci.postWorkerJob([&] {
        jobThread.store(QThread::currentThread());
        QThread::msleep(150);
        ++jobsDone;
    }));
    ASSERT_TRUE(ci.postWorkerJob([&] { ++jobsDone; }));
    ASSERT_TRUE(ci.postWorkerJob([&] { ++jobsDone; }));
    ASSERT_TRUE(waitFor([&] { return hookRuns.load() >= 1; })) << "the hook never ran";
    QThread::msleep(50);
    EXPECT_EQ(hookRuns.load(), 1) << "the hook ran before the queued jobs finished";
    EXPECT_EQ(jobsDoneAtHook.load(), 3) << "the hook ran before the last job";
    EXPECT_EQ(hookThread.load(), jobThread.load()) << "the hook ran off the shared-lane worker";

    ASSERT_TRUE(ci.postWorkerJob([&] { bulkDone.store(true); },
                                 ClaudeIntegration::DispatchLane::Bulk));
    ASSERT_TRUE(waitFor([&] { return bulkDone.load(); }));
    QThread::msleep(50);
    EXPECT_EQ(hookRuns.load(), 1) << "a bulk-lane job ran the hook";
}

// INV-3 — both processes install the hook on their RemoteControl.
TEST(McpIdleCacheRelease, Inv3BothProcessesInstallTheHook) {
    const std::string mcpd = ants_test::stripComments(
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mcpdmain.cpp"));
    const std::string mw = ants_test::stripComments(
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/mainwindow.cpp"));
    for (const auto *src : {&mcpd, &mw}) {
        const char *name = src == &mcpd ? "mcpdmain.cpp" : "mainwindow.cpp";
        const std::string flat = ants_test::squashWhitespace(*src);
        EXPECT_NE(flat.find("setSharedLaneIdleHook("), std::string::npos)
            << name << " installs no shared-lane idle hook";
        EXPECT_NE(flat.find("releaseIdleCaches();"), std::string::npos)
            << name << "'s hook does not release the caches";
    }
}
