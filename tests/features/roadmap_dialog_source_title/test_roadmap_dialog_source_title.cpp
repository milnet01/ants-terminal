// ANTS-5368 — the roadmap dialog's title names the source its data came from.
// Contract: spec.md here.

#include "config.h"
#include "roadmapdialog.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include "../../_support/xdg_guard.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

const char *kRoadmap =
    "# ROADMAP\n"
    "\n"
    "## Now\n"
    "\n"
    "- \xF0\x9F\x93\x8B [ANTS-0001] **A planned thing.**\n"
    "  Layman: A thing.\n"
    "  Kind: fix.\n"
    "  Source: seed.\n";

// Both XDG roots go to the temp dir: the dialog opens the roadmap store, and
// the store's default path would otherwise be the machine's real one.
struct Harness {
    ants_test::XdgGuard guard;
    QTemporaryDir       dir;
    QString             root;
    QString             path;

    Harness() {
        guard.setTestMode(false);
        guard.setEnv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toUtf8());
        guard.setEnv("XDG_DATA_HOME", dir.filePath(QStringLiteral("data")).toUtf8());
        root = QFileInfo(dir.path()).canonicalFilePath();
        path = root + QStringLiteral("/ROADMAP.md");
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(kRoadmap);
    }

    // Load the roadmap into the sandboxed store, as a migration does.
    bool migrate() const {
        RoadmapStore store(RoadmapStore::defaultPath(),
                           RoadmapStore::kDefaultHistoryCapBytes,
                           RoadmapStore::Access::Bulk);
        QString err;
        if (!store.open(&err)) { ADD_FAILURE() << err.toStdString(); return false; }
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        if (!disc) { ADD_FAILURE() << err.toStdString(); return false; }
        const auto plan = RoadmapMigrate::planFrom(
            *disc, QStringLiteral("Demo"), QStringLiteral("demo"));
        RoadmapMigrateLoad::Options opts;
        opts.changedAt   = QStringLiteral("2026-09-25T10:00:00Z");
        opts.projectRoot = root;
        const auto out = RoadmapMigrateLoad::load(store, plan, opts);
        if (!out.ok) ADD_FAILURE() << out.error.toStdString();
        return out.ok;
    }
};

// Let construction's first rebuild and its async follow-ups settle.
void settle(RoadmapDialog &dlg) {
    QElapsedTimer t;
    t.start();
    while ((dlg.recentCommitsInFlight() || dlg.lastTouchBlameInFlight())
           && t.elapsed() < 30000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    t.restart();
    while (t.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

}  // namespace

// INV-1
TEST(RoadmapDialogSourceTitle, Inv1FileProjectNamesTheFile) {
    Harness h;
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    settle(dlg);
    EXPECT_EQ(dlg.windowTitle(), QStringLiteral("Roadmap — from ROADMAP.md"));
}

// INV-2
TEST(RoadmapDialogSourceTitle, Inv2StoreProjectNamesTheStore) {
    Harness h;
    ASSERT_TRUE(h.migrate());
    Config cfg;
    RoadmapDialog dlg(h.path, QStringLiteral("light"), nullptr, &cfg);
    settle(dlg);
    EXPECT_EQ(dlg.windowTitle(), QStringLiteral("Roadmap — from the roadmap store"));
}
