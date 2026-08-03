// ANTS-3806 — where does an ARCHIVE's pre-heading content end up?
//
// The bullet's diagnosis was read off the code, not run: the synthetic root was
// believed to be one row per PROJECT (empty slug under UNIQUE (project_id,
// slug)), so every source's preamble would collapse into one row and an
// archive's own header would be lost. This case runs the real thing — two
// source files on disk, findRoadmaps() → planFrom() → load() → render() — and
// asserts the property that matters: each file's preamble comes back out of the
// file it went in from.
//
// Behavioural end to end, against a real store in a QTemporaryDir. The store is
// a unique_ptr and never a value member: a default-constructed RoadmapStore
// resolves defaultPath(), which is the developer's REAL store.

#include <gtest/gtest.h>

#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <memory>

namespace {

// Two ants-v1 sources, each with content ABOVE its first heading: the format
// marker, an H1 of its own, and one line of prose that exists in no other file.
// The prose lines are the probes — nothing else in either file carries them.
constexpr const char *kLive =
    "<!-- ants-roadmap-format: 1 -->\n"
    "\n"
    "# Demo — Roadmap\n"
    "\n"
    "Live preamble prose.\n"
    "\n"
    "## Now\n"
    "\n"
    "- 📋 [DEMO-0001] **A live item.**\n"
    "  Layman: A live thing.\n"
    "  Kind: implement.\n"
    "  Source: test.\n";

constexpr const char *kArchive =
    "<!-- ants-roadmap-format: 1 -->\n"
    "\n"
    "# Demo 0.7 — Roadmap Archive\n"
    "\n"
    "Archive preamble prose.\n"
    "\n"
    "## Shipped in 0.7\n"
    "\n"
    "- ✅ [DEMO-0002] **An archived item.**\n"
    "  Layman: An old thing.\n"
    "  Kind: implement.\n"
    "  Source: test.\n";

// An archive whose first line IS its first heading. `walkSource()` drops a
// synthetic root the source put nothing into, so this file reaches the store
// with no root section — the ONE case that leaves the render nothing to replay.
constexpr const char *kBareArchive =
    "## Shipped in 0.6\n"
    "\n"
    "- ✅ [DEMO-0003] **A bare archived item.**\n"
    "  Layman: An older thing.\n"
    "  Kind: implement.\n"
    "  Source: test.\n";

bool writeFile(const QString &path, const char *text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(QByteArray(text)) > 0;
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

TEST(RoadmapMigrateArchiveRoot, ArchivePreambleSurvivesMigrationAndRender) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), kLive));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/roadmap/0.7.md"), kArchive));

    // --- read half -------------------------------------------------------
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc) << err.toStdString();
    ASSERT_EQ(disc->sources.size(), 2);

    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));

    QVector<RoadmapMigrate::PlannedSection> roots;
    for (const RoadmapMigrate::PlannedSection &s : plan.sections)
        if (s.level == 0)
            roots.append(s);
    // One root per source that has pre-heading content — the property the
    // collapse would deny.
    ASSERT_EQ(roots.size(), 2) << "planned level-0 sections";
    EXPECT_EQ(roots.at(0).sourceIndex, 0);
    EXPECT_EQ(roots.at(1).sourceIndex, 1);
    EXPECT_NE(roots.at(0).slug, roots.at(1).slug)
        << "both roots planned under the same slug — UNIQUE (project_id, slug) "
           "would collapse them";
    EXPECT_TRUE(roots.at(0).intro.contains(QStringLiteral("Live preamble prose")));
    EXPECT_TRUE(roots.at(1).intro.contains(QStringLiteral("Archive preamble prose")));

    // --- load half -------------------------------------------------------
    // Access::Bulk — a migration load on an Interactive connection is refused.
    auto store = std::make_unique<RoadmapStore>(dir.filePath(QStringLiteral("store.db")),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                RoadmapStore::Access::Bulk);
    ASSERT_TRUE(store->open(&err)) << err.toStdString();

    RoadmapMigrateLoad::Options opts;
    opts.changedAt = QStringLiteral("2026-08-03T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    const auto stored = store->listSections(out.projectId, &err);
    ASSERT_TRUE(stored) << err.toStdString();
    QVector<RoadmapStore::SectionRow> storedRoots;
    for (const RoadmapStore::SectionRow &s : *stored)
        if (s.level == 0)
            storedRoots.append(s);
    ASSERT_EQ(storedRoots.size(), 2) << "stored level-0 sections";

    bool sawLive = false, sawArchive = false;
    for (const RoadmapStore::SectionRow &s : storedRoots) {
        if (s.intro.contains(QStringLiteral("Live preamble prose"))) {
            sawLive = true;
            EXPECT_FALSE(s.sourcePath) << "the live root must store NULL";
        }
        if (s.intro.contains(QStringLiteral("Archive preamble prose"))) {
            sawArchive = true;
            ASSERT_TRUE(s.sourcePath);
            EXPECT_EQ(*s.sourcePath, QStringLiteral("docs/roadmap/0.7.md"));
        }
    }
    EXPECT_TRUE(sawLive) << "the live preamble is not in the store";
    EXPECT_TRUE(sawArchive) << "the archive preamble is not in the store";

    // --- render half -----------------------------------------------------
    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto rendered = RoadmapRender::render(*store, out.projectId, root, ropts, &err);
    ASSERT_TRUE(rendered) << err.toStdString();
    ASSERT_TRUE(rendered->gateFailures.isEmpty())
        << rendered->gateFailures.join(QStringLiteral(", ")).toStdString();
    ASSERT_TRUE(rendered->committed);

    const QString live = readAll(root + QStringLiteral("/ROADMAP.md"));
    const QString archive = readAll(root + QStringLiteral("/docs/roadmap/0.7.md"));
    EXPECT_TRUE(live.contains(QStringLiteral("Live preamble prose")));
    EXPECT_TRUE(live.contains(QStringLiteral("# Demo — Roadmap")));
    EXPECT_TRUE(archive.contains(QStringLiteral("Archive preamble prose")))
        << "the archive's own preamble was not replayed:\n" << archive.toStdString();
    EXPECT_TRUE(archive.contains(QStringLiteral("# Demo 0.7 — Roadmap Archive")))
        << "the archive's own H1 was not replayed:\n" << archive.toStdString();
    // Neither file may acquire the other's header.
    EXPECT_FALSE(archive.contains(QStringLiteral("Live preamble prose")));
    EXPECT_FALSE(live.contains(QStringLiteral("Archive preamble prose")));
}

// The complement, and the case that keeps ANTS-3758's constant marker earning
// its place: an archive that opens on a heading contributes no root section, so
// the render has nothing to replay and must supply the marker itself. Without
// this the file would leave the store unparseable as a conforming roadmap.
TEST(RoadmapMigrateArchiveRoot, ArchiveWithNoPreambleStillGetsTheFormatMarker) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), kLive));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/docs/roadmap/0.6.md"), kBareArchive));

    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc) << err.toStdString();
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    for (const RoadmapMigrate::PlannedSection &s : plan.sections)
        EXPECT_FALSE(s.level == 0 && s.sourceIndex == 1)
            << "a source with no pre-heading content planned a root anyway";

    auto store = std::make_unique<RoadmapStore>(dir.filePath(QStringLiteral("store.db")),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                RoadmapStore::Access::Bulk);
    ASSERT_TRUE(store->open(&err)) << err.toStdString();
    RoadmapMigrateLoad::Options opts;
    opts.changedAt = QStringLiteral("2026-08-03T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    ASSERT_TRUE(out.ok) << out.error.toStdString();

    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto rendered = RoadmapRender::render(*store, out.projectId, root, ropts, &err);
    ASSERT_TRUE(rendered) << err.toStdString();
    ASSERT_TRUE(rendered->committed);

    const QString archive = readAll(root + QStringLiteral("/docs/roadmap/0.6.md"));
    // Exactly once: supplied, not duplicated.
    EXPECT_EQ(archive.count(QStringLiteral("ants-roadmap-format")), 1)
        << archive.toStdString();
    EXPECT_TRUE(archive.startsWith(QStringLiteral("<!-- ants-roadmap-format: 1 -->")))
        << archive.toStdString();
}
