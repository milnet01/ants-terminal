// Feature-conformance test for ANTS-3761 INV-9 — the export's write lock.
// Contract: tests/features/roadmap_export_concurrency/spec.md

#include <gtest/gtest.h>

#include "configbackup.h"
#include "roadmapexport.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

// Anything sharing the destination's name prefix except the export itself and
// ConfigWriteLock's own sibling lock file. QSaveFile names its temp
// `<dest>.XXXXXX`, so a writer that opened one and abandoned it shows up here.
// Prefix-scoped rather than "every file in the directory": the store's own
// roadmap.sqlite and its -wal / -shm siblings live here too and are not strays.
QStringList strayFiles(const QString &dirPath, const QString &destName) {
    QStringList stray;
    for (const QString &name :
         QDir(dirPath).entryList(QDir::Files | QDir::Hidden | QDir::System))
        if (name.startsWith(destName) && name != destName &&
            name != destName + QStringLiteral(".lock"))
            stray << name;
    return stray;
}

}  // namespace

// INV-9 — every export write path acquires ConfigWriteLock, and ABORTS LOUDLY
// when it cannot.
//
// Phrasing matters, and the test is written to the phrasing: flock(2) is
// advisory, so a non-cooperating writer can still interleave. The testable
// claim is about OUR writer, not about the file — that it refuses rather than
// treating a failed acquire as permission to proceed unprotected.
TEST(RoadmapExportConcurrency, Inv9AbortsWhenTheLockIsHeld) {
    QTemporaryDir dir;
    RoadmapStore store(dir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();

    const QString root = dir.path() + QStringLiteral("/alpha");
    QDir().mkpath(root);
    ASSERT_TRUE(store.registerProject(root, QStringLiteral("Alpha"), QStringLiteral("alpha"), &err)
                    .has_value())
        << err.toStdString();

    const QString destName = QStringLiteral("alpha.jsonl");
    const QString dest = dir.path() + QStringLiteral("/") + destName;

    {
        // flock is per open file description, so a second ConfigWriteLock in
        // this same process contends exactly as another Ants instance would.
        ConfigWriteLock held(dest);
        ASSERT_TRUE(held.acquired());

        err.clear();
        EXPECT_FALSE(RoadmapExport::exportProject(store, QStringLiteral("alpha"), dest, &err))
            << "a failed acquire must abort the export, not proceed unprotected";
        EXPECT_FALSE(err.isEmpty()) << "aborting silently is the failure the model's § 9 names";

        // Wrote no bytes, and left no temp file. A truncated backup that looks
        // complete is the failure this whole spec exists to prevent, and an
        // orphaned temp beside it is the same lie one directory entry over.
        EXPECT_FALSE(QFile::exists(dest));
        EXPECT_EQ(strayFiles(dir.path(), destName), QStringList())
            << "the aborted export left a temp file behind";
    }

    // Released — the same call now succeeds, which is what proves the refusal
    // above was the lock and not some unrelated failure.
    err.clear();
    EXPECT_TRUE(RoadmapExport::exportProject(store, QStringLiteral("alpha"), dest, &err))
        << err.toStdString();
    EXPECT_TRUE(QFile::exists(dest));
    EXPECT_GT(QFileInfo(dest).size(), 0);
    EXPECT_EQ(strayFiles(dir.path(), destName), QStringList())
        << "a successful export left a temp file behind";
}
