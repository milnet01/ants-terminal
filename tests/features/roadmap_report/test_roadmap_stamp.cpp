// Feature-conformance test for ANTS-4501 slice 2 — forward stamping.
// Contract: docs/specs/ANTS-4501-roadmap-report.md § 2.2.
//
// Covers INV-5 (`shipped` is stamped on the transition INTO shipped, never on a
// write to an already-shipped item) and INV-6 (`shipped` is cleared on the
// transition out). Both need the VERB path, not the store: § 2.2 puts the stamp
// in the callers precisely so `RoadmapStore::setItemField()` stays neutral, so a
// store-level test would assert against the wrong layer. That is why this file
// sits in the GUI bundle beside test_roadmap_report.cpp's core-bundle cases.
//
// Both clauses turn on the day ADVANCING between two writes. On one real clock
// the two writes land on the same date, the assertion holds, and the case passes
// against exactly the broken build it exists to catch — which is why § 2.2
// requires RoadmapClock and why every case here drives it.
//
// Harness copied in shape from tests/features/roadmap_write_history/: XDG
// redirected into a QTemporaryDir, a small fixture migrated into a store at
// RoadmapStore::defaultPath(), then the real verbs driven and the item columns
// read back by raw SELECT so NULL is distinguishable from empty.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapclock.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QVariant>

#include <memory>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Safe here only because XdgGuard has already
// redirected that variable into the temporary directory.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath(),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Restores the real clock however the case leaves — a failed EXPECT must not
// leak a frozen date into the next test in the binary.
struct ClockGuard {
    ~ClockGuard() { RoadmapClock::setTodayForTest(QDate()); }
};

// Past kRoadmapMinParseableSize (1024 B): below it the write paths refuse
// unrecognised_format before any locator is tried.
const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla\n"
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
    "qui officia deserunt mollit anim id est laborum. Sed ut perspiciatis unde\n"
    "omnis iste natus error sit voluptatem accusantium doloremque laudantium,\n"
    "totam rem aperiam eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo nemo enim ipsam voluptatem.\n";

QByteArray fixture() {
    QByteArray b =
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
         "## Work\n"
         "\n"
         "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
         "  Layman: A thing.\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n"
         "- \xF0\x9F\x93\x8B [DEMO-0008] **A second open item.**\n"
         "  Layman: Another thing.\n"
         "  Kind: fix.\n"
         "  Source: seed.\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    // The store keys a project on its CANONICAL root (ANTS-3756 INV-8), and
    // /tmp is a symlink on some hosts.
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return QString();
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-14T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    *projectId = out.projectId;
    return root;
}

// The three date columns, read raw so a SQL NULL is distinguishable from the
// empty string readItem() would hand back for both.
struct Dates {
    bool found = false;
    bool createdNull = true, modifiedNull = true, shippedNull = true;
    QString created, lastModified, shipped;
};

Dates datesOf(qint64 projectId, const QString &id) {
    Dates d;
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return d;
    QSqlQuery q(store->db());
    q.prepare(QStringLiteral(
        "SELECT created, last_modified, shipped FROM item "
        "WHERE project_id = ? AND id = ?"));
    q.addBindValue(projectId);
    q.addBindValue(id);
    if (!q.exec()) {
        ADD_FAILURE() << "item SELECT: " << q.lastError().text().toStdString();
        return d;
    }
    if (!q.next()) return d;
    d.found        = true;
    d.createdNull  = q.value(0).isNull();
    d.modifiedNull = q.value(1).isNull();
    d.shippedNull  = q.value(2).isNull();
    d.created      = q.value(0).toString();
    d.lastModified = q.value(1).toString();
    d.shipped      = q.value(2).toString();
    return d;
}

QJsonObject runFlip(const QString &root, const QString &id, const QString &toStatus) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = toStatus;
    req[QStringLiteral("id")]         = id;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogFlipForTest(req).object();
}

QJsonObject runAnnotate(const QString &root, const QString &id, const QString &note) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("annotate");
    req[QStringLiteral("id")]         = id;
    req[QStringLiteral("note")]       = note;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogFlipForTest(req).object();
}

#define ASSERT_VERB_OK(resp)                                                    \
    ASSERT_TRUE((resp).value(QStringLiteral("ok")).toBool())                     \
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString()

}  // namespace

// ------------------------------------------------------------------ INV-5 ---
//
// The trap: attach the stamp to "status IS shipped" rather than "status BECAME
// shipped" and one re-render dates the whole backlog to today. Every throughput
// figure is then wrong in the same direction, which is what makes it hard to
// notice at corpus scale and invisible in a one-item unit test.
TEST(RoadmapStamp, Inv5ShippedStampedOnTransitionInNotOnLaterWrites) {
    ants_test::XdgGuard guard;
    ClockGuard clock;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    // § 2.2 — migration does not stamp, on an insert or an update. Asserted
    // rather than assumed: if the loader stamped, every date below would be
    // non-NULL already and the case would pass without the feature.
    const Dates seeded = datesOf(projectId, QStringLiteral("DEMO-0007"));
    ASSERT_TRUE(seeded.found) << "the fixture item is not in the store";
    EXPECT_TRUE(seeded.createdNull)  << "migration stamped `created`";
    EXPECT_TRUE(seeded.modifiedNull) << "migration stamped `last_modified`";
    EXPECT_TRUE(seeded.shippedNull)  << "migration stamped `shipped`";

    RoadmapClock::setTodayForTest(QDate(2026, 3, 1));
    ASSERT_VERB_OK(runFlip(root, QStringLiteral("DEMO-0007"), QStringLiteral("shipped")));
    const Dates closed = datesOf(projectId, QStringLiteral("DEMO-0007"));
    ASSERT_FALSE(closed.shippedNull) << "the transition into shipped set no date";
    EXPECT_EQ(closed.shipped.toStdString(), std::string("2026-03-01"));
    EXPECT_EQ(closed.lastModified.toStdString(), std::string("2026-03-01"));

    // The day advances. Without this the two writes land on one date and the
    // assertions below hold against the build they exist to catch.
    RoadmapClock::setTodayForTest(QDate(2026, 3, 2));

    // (a) a body write to an already-shipped item.
    ASSERT_VERB_OK(runAnnotate(root, QStringLiteral("DEMO-0007"),
                               QStringLiteral("Progress (2026-03-02): a later note.")));
    Dates after = datesOf(projectId, QStringLiteral("DEMO-0007"));
    EXPECT_EQ(after.shipped.toStdString(), std::string("2026-03-01"))
        << "a body write moved the closure date";
    EXPECT_EQ(after.lastModified.toStdString(), std::string("2026-03-02"))
        << "`last_modified` did not follow the body write";

    // (b) a status write whose new value is shipped and whose OLD value already
    // was — the re-render case, and the one that costs the whole backlog.
    ASSERT_VERB_OK(runFlip(root, QStringLiteral("DEMO-0007"), QStringLiteral("shipped")));
    after = datesOf(projectId, QStringLiteral("DEMO-0007"));
    EXPECT_EQ(after.shipped.toStdString(), std::string("2026-03-01"))
        << "a shipped -> shipped write moved the closure date";
}

// ------------------------------------------------------------------ INV-6 ---
//
// The first assertion is what makes this clause mean anything: without it the
// fixture reads NULL before and NULL after, and passes against a build that
// implements neither half of § 2.2's `shipped` rule.
TEST(RoadmapStamp, Inv6ShippedClearedOnTransitionOut) {
    ants_test::XdgGuard guard;
    ClockGuard clock;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RoadmapClock::setTodayForTest(QDate(2026, 3, 1));
    ASSERT_VERB_OK(runFlip(root, QStringLiteral("DEMO-0008"), QStringLiteral("shipped")));
    const Dates closed = datesOf(projectId, QStringLiteral("DEMO-0008"));
    ASSERT_TRUE(closed.found);
    ASSERT_FALSE(closed.shippedNull)
        << "nothing was set, so 'cleared' below would pass for the wrong reason";
    EXPECT_EQ(closed.shipped.toStdString(), std::string("2026-03-01"));

    RoadmapClock::setTodayForTest(QDate(2026, 3, 2));
    ASSERT_VERB_OK(runFlip(root, QStringLiteral("DEMO-0008"), QStringLiteral("planned")));
    const Dates reopened = datesOf(projectId, QStringLiteral("DEMO-0008"));
    EXPECT_TRUE(reopened.shippedNull)
        << "a reopened item still carries a closure date, so it counts as closed "
           "in every period report thereafter";
    EXPECT_EQ(reopened.lastModified.toStdString(), std::string("2026-03-02"));
}
