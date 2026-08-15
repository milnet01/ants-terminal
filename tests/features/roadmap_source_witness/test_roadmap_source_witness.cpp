// Feature-conformance test for ANTS-4402 — the roadmap source witness.
// Contract: tests/features/roadmap_source_witness/spec.md
//
// Behavioural through RemoteControl::cmdRoadmapQuery against real fixture
// trees. XDG_DATA_HOME is redirected per case so RoadmapStore::defaultPath()
// — which cmdRoadmapQuery resolves internally and no argument overrides —
// lands in a QTemporaryDir instead of the developer's REAL store.

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &text) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(text) == text.size();
}

// An ants-v1 roadmap: the dialect gate in RoadmapSource::migratedProject()
// serves the store only for this format, so a fixture without the marker would
// silently exercise the markdown branch in every case.
QByteArray roadmapText() {
    return "<!-- ants-roadmap-format: 1 -->\n"
           "\n"
           "# Demo — Roadmap\n"
           "\n"
           "## Work\n"
           "\n"
           "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n";
}

// The bullet a session files BY HAND under ANTS-4141's workaround: a real id,
// far above anything the migration imported, written straight into the file.
QByteArray handFiledBullet() {
    return "\n- \xF0\x9F\x93\x8B [DEMO-4242] **Filed by hand, after the migration.**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n";
}

// Redirects XDG_DATA_HOME for its lifetime and restores the prior value —
// including its ABSENCE, which is a different state from empty: leaving an
// empty XDG_DATA_HOME set would point every later test in this binary at the
// filesystem root rather than at the user's real data dir.
class XdgRedirect {
public:
    explicit XdgRedirect(const QString &dir)
        : m_had(qEnvironmentVariableIsSet("XDG_DATA_HOME")),
          m_prior(m_had ? qgetenv("XDG_DATA_HOME") : QByteArray()) {
        qputenv("XDG_DATA_HOME", dir.toLocal8Bit());
    }
    ~XdgRedirect() {
        if (m_had)
            qputenv("XDG_DATA_HOME", m_prior);
        else
            qunsetenv("XDG_DATA_HOME");
    }
    XdgRedirect(const XdgRedirect &) = delete;
    XdgRedirect &operator=(const XdgRedirect &) = delete;

private:
    bool m_had;
    QByteArray m_prior;
};

// findRoadmaps → planFrom → load, the migration as a consumer runs it. Bulk,
// because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12).
bool migrateDefaultStore(const QString &root) {
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
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                 QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt = QStringLiteral("2026-08-15T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

// A FRESH RemoteControl per query: cmdRoadmapQuery memoises bullets on
// (path, mtime) with a 100 ms TTL, and mtime has 1-second resolution on some
// filesystems — so a case that rewrites the fixture and reuses one instance
// can read its own stale cache and pass for the wrong reason.
QJsonObject query(const QString &root) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapSourceWitness, Inv1MarkdownSource) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("markdown"))
        << "no store on this machine — the file IS the answer";
    // The markdown backend cannot be stale against itself, so none of the
    // warning triple may appear.
    EXPECT_FALSE(out.contains(QStringLiteral("file_ahead_of_store")));
    EXPECT_FALSE(out.contains(QStringLiteral("file_highest_id")));
    EXPECT_FALSE(out.contains(QStringLiteral("store_high_water")));
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapSourceWitness, Inv2StoreSource) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "a migrated project is served by the store, and must say so — this "
           "field sits beside a `path` naming a file the answer did not come "
           "from, which is the whole defect";
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapSourceWitness, Inv3FileAheadOfStoreWarns) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const XdgRedirect xdg(dir.filePath(QStringLiteral("xdg")));

    const QString root = dir.filePath(QStringLiteral("proj"));
    const QString rmPath = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(writeFile(rmPath, roadmapText()));
    ASSERT_TRUE(migrateDefaultStore(root));

    // The ANTS-4141 workaround, performed literally: append a bullet to the
    // file the store has already been built from. Nothing re-imports it.
    ASSERT_TRUE(writeFile(rmPath, roadmapText() + handFiledBullet()));

    const QJsonObject out = query(root);
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool()) << "query refused";
    EXPECT_EQ(out.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"));
    EXPECT_TRUE(out.value(QStringLiteral("file_ahead_of_store")).toBool())
        << "DEMO-4242 is in the file and not in the store; a reader given "
           "ok:true and no warning has no way to learn that";
    EXPECT_EQ(out.value(QStringLiteral("file_highest_id")).toInt(), 4242);
    EXPECT_LT(out.value(QStringLiteral("store_high_water")).toInt(), 4242)
        << "both numbers are reported so the gap is visible, not inferred";
}
