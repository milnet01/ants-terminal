// Feature-conformance test for ANTS-4493 — the markdown allocation path floors
// to the STORE's id high-water. See
// tests/features/roadmap_alloc_store_floor/spec.md.
//
// The two allocation paths each floor to two of the three places an id can
// already exist, and a migrated project that is NOT served from the store takes
// the one that cannot see the store. Its synthesised ids live nowhere else, so
// the first append after the migration reissues one.

#include "../../_support/xdg_guard.h"

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

#include <memory>

namespace {

// A MIXED roadmap, which is the reported shape: the majority dialect is
// github-task-list, so migratedProject() returns nullopt and roadmap_log takes
// the markdown path — while the file still carries the one ants-v1 bullet that
// gives the prefix and the file-side high-water.
const char *kMixedRoadmap =
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- [x] A github-task-list bullet with no id.\n"
    "- [ ] Another one with no id.\n"
    "- \xF0\x9F\x93\x8B [DEMO-0001] **The one bullet roadmap_log wrote.**\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Every case here redirects that first.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Writes the fixture and returns the CANONICAL root (the store keys a project
// on it, ANTS-3756 INV-8, and /tmp is a symlink on some hosts).
QString seedProject(ants_test::XdgGuard &guard, const QTemporaryDir &tmp) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), QByteArray(kMixedRoadmap)))
        return QString();
    // The counter LAGS the file, which is the reported state (Vestige's sat at
    // 47 while its ids reached 0611). Without the file at all the verb refuses
    // `stable_prefix_unsupported`, reading DEMO-0001 as a stable-string id.
    if (!writeFile(raw + QStringLiteral("/.roadmap-counter"), QByteArray("1\n")))
        return QString();
    return QFileInfo(raw).canonicalFilePath();
}

// Runs the migration, which synthesises an id for each of the two id-less
// bullets and raises the store's high-water past the file's only declared id.
bool migrate(const QString &root, qint64 *allocated) {
    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store)
        return false;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-19T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    *allocated = out.idsAllocated;
    return true;
}

QJsonObject appendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   = QStringLiteral("The first append after migrating.");
    // Required, not decoration: the render's INV-5 gate refuses a project whose
    // open items carry no Layman line, and on the store path that refusal is
    // reached before anything is written (render_gate_unmet).
    o[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    o[QStringLiteral("kind")]       = QStringLiteral("chore");
    o[QStringLiteral("source")]     = QStringLiteral("ants-4493-test");
    o[QStringLiteral("id_prefix")]  = QStringLiteral("DEMO");
    return o;
}

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(RoadmapAllocStoreFloor, Inv1AppendDoesNotReissueASynthesisedId) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());

    qint64 allocated = 0;
    ASSERT_TRUE(migrate(root, &allocated));
    ASSERT_EQ(allocated, 2)
        << "the fixture's two id-less bullets must be synthesised, or this case "
           "has no stored id to collide with";

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    // The file's only declared id is DEMO-0001, so the file+corpus floor alone
    // issues DEMO-0002 — which the migration has already given to one of the
    // id-less bullets, in the store and in no file.
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0004"))
        << "the allocator reissued an id the store already holds";
}

// ----------------------------------------------------------------- INV-2 ----
//
// The floor is additive. A project with no store row must allocate exactly as
// it did before, or the fix would have moved every project's numbering.

TEST(RoadmapAllocStoreFloor, Inv2UnmigratedProjectAllocatesUnchanged) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0002"))
        << "with no store row the file's high-water is the only floor";
}

// ----------------------------------------------------------------- INV-3 ----
//
// ANTS-4141 part 2. The store path allocates from the store's high-water and
// never wrote .roadmap-counter, so the counter drifted one per allocation —
// measured 2026-08-19 on this project at 4402 against a store high-water of
// 4501. Anything that still READS the counter (a fresh clone, a hand append, a
// tool) then allocates into a block the store already owns.
//
// Reconciled on every store-path allocation rather than only after a
// migration: the counter is a derived cache (roadmap-format.md § 3.5.1), and a
// cache refreshed only at migration time starts drifting again with the next
// append.

namespace {

// An ants-v1 roadmap, so migratedProject() resolves and roadmap_log takes the
// STORE path — the opposite of the mixed fixture above, and the distinction the
// whole file turns on.
const char *kAntsV1Roadmap =
    "<!-- ants-roadmap-format: 1 -->\n"
    "\n"
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0001] **A seed bullet.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

qint64 readCounter(const QString &root) {
    QFile f(QDir(root).filePath(QStringLiteral(".roadmap-counter")));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}

}  // namespace

TEST(RoadmapAllocStoreFloor, Inv3StorePathReconcilesTheCounterCache) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    ASSERT_TRUE(writeFile(rawRoot + QStringLiteral("/ROADMAP.md"),
                          QByteArray(kAntsV1Roadmap)));
    // The counter LAGS, which is the state this invariant exists to end.
    ASSERT_TRUE(writeFile(rawRoot + QStringLiteral("/.roadmap-counter"),
                          QByteArray("1\n")));
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    qint64 allocated = 0;
    ASSERT_TRUE(migrate(root, &allocated));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    // The store path is the one under test. If the project resolved to the
    // markdown path this case would pass for the wrong reason, and the tell is
    // the envelope: only the markdown path reports a line number.
    ASSERT_FALSE(resp.contains(QStringLiteral("line")))
        << "this fixture took the MARKDOWN path, so it is not testing the store "
           "path's counter write: " << QJsonDocument(resp).toJson().toStdString();

    const QString id = resp.value(QStringLiteral("id")).toString();
    ASSERT_EQ(id, QStringLiteral("DEMO-0002"));
    EXPECT_EQ(readCounter(root), 2)
        << "the store path allocated " << id.toStdString()
        << " and left the counter cache behind at " << readCounter(root);
}
