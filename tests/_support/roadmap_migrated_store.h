// A test's route to a project the roadmap store serves. The roadmap_log
// handlers open RoadmapStore::defaultPath() themselves, and no argument
// overrides it, so a test redirects XDG_DATA_HOME into a temporary directory
// first; without that it would write the real machine-global store.
//
//   QTemporaryDir xdg, proj;
//   ants_test::XdgGuard env;
//   env.setEnv("XDG_DATA_HOME", xdg.path().toLocal8Bit());
//   // write an ants-v1 ROADMAP.md under proj, then:
//   ASSERT_TRUE(ants_test::migrateDefaultStore(proj.path()));
//
// The roadmap must be ants-v1: RoadmapSource::migratedProject() serves the
// store only for that dialect, so a plain markdown fixture would silently
// take the file path.
#pragma once

#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace ants_test {

// Bulk, because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12).
inline bool migrateDefaultStore(const QString &root) {
    const QString dbPath = RoadmapStore::defaultPath();
    QDir().mkpath(QFileInfo(dbPath).path());
    RoadmapStore store(dbPath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    if (!store.open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return false;
    }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-24T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

}  // namespace ants_test
