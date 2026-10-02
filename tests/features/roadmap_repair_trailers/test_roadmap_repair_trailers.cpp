// ANTS-4585 phase 2 — repair the truncated trailer columns by re-parse.
// Contract: tests/features/roadmap_repair_trailers/spec.md
//
// Behavioural, against a migrated store: a fixture whose bullets carry the
// two truncation shapes plus the cases the guard must refuse to touch.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"
#include "../../_support/roadmapstoreaccess.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
#include <string>

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
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

// Every bullet keeps its legacy inline run MID-BODY, with a prose line after
// it. That placement is load-bearing: migration strips a run that TRAILS the
// body, and a fixture whose run is stripped reproduces the one state this pass
// cannot repair (the prose is the only surviving copy) rather than the state it
// exists for. Measured on the first draft — `items_with_run` came back 1 of 4.
QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0001] **An abbreviation-stop truncation.**\n"
        "  **Layman:** A plain config.yaml file gets no checking at all.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  A closing line, so the run above does not trail the body.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0002] **A hard-wrap truncation.**\n"
        "  Lanes: build, ci, tests, security.\n"
        "  **Layman:** The lane list lost its last member at the wrap.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  A closing line, so the run above does not trail the body.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0003] **Already whole.**\n"
        "  **Layman:** Nothing here was ever cut.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "  A closing line, so the run above does not trail the body.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0004] **No inline run at all.**\n"
        "  Just prose, and not a trailer key in sight.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}

// Write the short value migration WOULD have left before ANTS-4542 / ANTS-4596
// / ANTS-4597. The causes are fixed, so migrating this fixture stores the right
// answer and there is nothing to repair — a test that skipped this step would
// pass while exercising none of the pass, which the first draft of INV-2 did.
bool damage(qint64 projectId, const QString &id, const QString &field,
            const QString &shortValue) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return false;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) { ADD_FAILURE() << "findItem " << id.toStdString(); return false; }
    if (!store->setItemField(*pk, field, shortValue, &err)) {
        ADD_FAILURE() << "damage " << field.toStdString() << ": " << err.toStdString();
        return false;
    }
    return true;
}

QJsonObject repair(RemoteControl &rc, const QString &root, bool dryRun,
                   bool stripRuns = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("repair_trailers");
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    if (stripRuns) req[QStringLiteral("strip_runs")] = true;
    return rc.cmdRoadmapLogRepairTrailersForTest(req).object();
}

// Read a column straight from the store — never through roadmap_query, whose
// `body` composes a trailer line from the column (ANTS-4599) and so cannot
// answer a question about the column.
QString columnOf(qint64 projectId, const QString &id, const QString &field) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) { ADD_FAILURE() << "findItem " << id.toStdString(); return QString(); }
    const auto it = store->readItem(*pk, &err);
    if (!it) { ADD_FAILURE() << "readItem " << id.toStdString(); return QString(); }
    if (field == QLatin1String("layman")) return it->layman;
    if (field == QLatin1String("lanes"))  return it->lanes.join(QLatin1Char('|'));
    if (field == QLatin1String("source")) return it->source;
    if (field == QLatin1String("kind"))   return it->kind;
    if (field == QLatin1String("body"))   return it->body;
    return QString();
}

// ANTS-4507 — the legacy state: a stored body that ENDS in a trailer run. The
// migration strips a trailing run since ANTS-4506, so a fixture migrated today
// never holds one; it is written back here, the way `damage()` injects a short
// column. DEMO-0004's columns after migration are kind `fix`, source `seed` and
// no layman, so the run below agrees with them once its layman is stored too.
const char *kProse = "Just prose, and not a trailer key in sight.";

bool injectTrailingRun(qint64 projectId, const QString &source) {
    return damage(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("layman"),
                  QStringLiteral("A short summary"))
        && damage(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"),
                  QString::fromUtf8(kProse)
                      + QStringLiteral("\n**Layman:** A short summary.\nKind: fix.\nSource: ")
                      + source + QStringLiteral("."));
}

bool idListed(const QJsonObject &resp, const char *key, const QString &id) {
    for (const auto &v : resp.value(QLatin1String(key)).toArray())
        if (v.toString() == id) return true;
    return false;
}

}  // namespace

// ------------------------------------------------------------- INV-1/2/4/8 --

TEST(RoadmapRepairTrailers, Inv1AbbreviationStopIsRepaired) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    // The migration-era state, injected because today's parser no longer
    // produces it. Asserted rather than assumed: without this check a green
    // repair could mean "nothing was broken".
    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0001"),
                       QStringLiteral("layman"), QStringLiteral("A plain config")));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0001"), QStringLiteral("layman")).toStdString(),
              std::string("A plain config"));

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, /*dryRun=*/false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0001"), QStringLiteral("layman")).toStdString(),
              std::string("A plain config.yaml file gets no checking at all"));
}

// RC-21 (audit 2026-09-26) — a repaired COLUMN is recorded in history, as a
// stripped body already was, so the value it replaced is recoverable.
TEST(RoadmapRepairTrailers, RepairedColumnIsRecordedInHistory) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0001"),
                       QStringLiteral("layman"), QStringLiteral("A plain config")));

    RemoteControl rc(nullptr);
    ASSERT_TRUE(repair(rc, root, false).value(QStringLiteral("ok")).toBool());

    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_TRUE(store);
    QString err;
    const auto pk = store->findItem(projectId, QStringLiteral("DEMO-0001"), &err);
    ASSERT_TRUE(pk);
    QSqlQuery q(RoadmapStoreTestAccess::db(*store));
    ASSERT_TRUE(q.prepare(QStringLiteral("SELECT old_value, new_value FROM history "
                                         "WHERE item_pk = ? AND field = 'layman'")));
    q.addBindValue(*pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next()) << "the layman repair left no history row";
    EXPECT_EQ(q.value(0).toString().toStdString(), std::string("A plain config"));
    EXPECT_EQ(q.value(1).toString().toStdString(),
              std::string("A plain config.yaml file gets no checking at all"));
    EXPECT_FALSE(q.next()) << "one repair, one history row";
}

TEST(RoadmapRepairTrailers, Inv2HardWrapIsRepaired) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0002"), QStringLiteral("lanes"),
                       QStringLiteral("[\"build\",\"ci\",\"tests\"]")));
    ASSERT_EQ(columnOf(projectId, QStringLiteral("DEMO-0002"), QStringLiteral("lanes")).toStdString(),
              std::string("build|ci|tests"));

    RemoteControl rc(nullptr);
    ASSERT_TRUE(repair(rc, root, false).value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0002"), QStringLiteral("lanes")).toStdString(),
              std::string("build|ci|tests|security"))
        << "the wrapped lane list did not regain its last member";
}

TEST(RoadmapRepairTrailers, Inv4IntactValueIsNotRewritten) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0003"), QStringLiteral("layman"));
    RemoteControl rc(nullptr);
    ASSERT_TRUE(repair(rc, root, false).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0003"), QStringLiteral("layman")).toStdString(),
              before.toStdString());
}

TEST(RoadmapRepairTrailers, Inv8ItemWithNoRunIsUntouched) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("layman"));
    RemoteControl rc(nullptr);
    ASSERT_TRUE(repair(rc, root, false).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("layman")).toStdString(),
              before.toStdString());
}

// ----------------------------------------------------------------- INV-3 ----

// The guard, and the reason the whole pass is shaped around it: a stored value
// that is NEWER than the prose must survive. Simulated the way it happens for
// real — the column is edited after migration and the legacy run is left alone.
TEST(RoadmapRepairTrailers, Inv3NonPrefixValueIsSkippedNotReverted) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString edited =
        QStringLiteral("A later, better sentence that shares no prefix.");
    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_TRUE(store != nullptr);
        QString err;
        const auto pk = store->findItem(projectId, QStringLiteral("DEMO-0001"), &err);
        ASSERT_TRUE(pk.has_value()) << err.toStdString();
        ASSERT_TRUE(store->setItemField(*pk, QStringLiteral("layman"), edited, &err))
            << err.toStdString();
    }

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0001"), QStringLiteral("layman")).toStdString(),
              edited.toStdString())
        << "the repair reverted a post-migration edit to stale prose";
    EXPECT_GE(resp.value(QStringLiteral("skipped")).toInt(), 1)
        << "a skip must be reported, not silent: " << QJsonDocument(resp).toJson().toStdString();
}

// ----------------------------------------------------------------- INV-5 ----

TEST(RoadmapRepairTrailers, Inv5DryRunWritesNothingAndPredictsTheRun) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0001"),
                       QStringLiteral("layman"), QStringLiteral("A plain config")));
    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0001"), QStringLiteral("layman"));

    RemoteControl rc(nullptr);
    const QJsonObject dry = repair(rc, root, /*dryRun=*/true);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(dry).toJson().toStdString();
    EXPECT_TRUE(dry.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0001"), QStringLiteral("layman")).toStdString(),
              before.toStdString())
        << "dry_run wrote to the store";

    const QJsonObject wet = repair(rc, root, /*dryRun=*/false);
    ASSERT_TRUE(wet.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(dry.value(QStringLiteral("repaired")).toInt(),
              wet.value(QStringLiteral("repaired")).toInt())
        << "the preview did not predict the run";
    EXPECT_GT(wet.value(QStringLiteral("repaired")).toInt(), 0);
}

// ----------------------------------------------------------------- INV-7 ----

TEST(RoadmapRepairTrailers, Inv7SecondRunRepairsNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0001"),
                       QStringLiteral("layman"), QStringLiteral("A plain config")));

    RemoteControl rc(nullptr);
    const QJsonObject first = repair(rc, root, false);
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool());
    EXPECT_GT(first.value(QStringLiteral("repaired")).toInt(), 0);

    const QJsonObject second = repair(rc, root, false);
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(second.value(QStringLiteral("repaired")).toInt(), 0)
        << "the pass is not idempotent: " << QJsonDocument(second).toJson().toStdString();
}

// ---------------------------------------------------------- INV-9..12 -------

TEST(RoadmapRepairTrailers, Inv9RedundantTrailingRunIsStripped) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(injectTrailingRun(projectId, QStringLiteral("seed")));
    ASSERT_NE(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              std::string(kProse)) << "the legacy run was not injected";

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false, /*stripRuns=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              std::string(kProse)) << "the redundant run was not stripped";
    EXPECT_EQ(resp.value(QStringLiteral("runs_stripped")).toInt(), 1)
        << QJsonDocument(resp).toJson().toStdString();
    // The columns the run duplicated are untouched.
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("layman")).toStdString(),
              std::string("A short summary"));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("kind")).toStdString(),
              std::string("fix"));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("source")).toStdString(),
              std::string("seed"));
    // A run that sits mid-body is not a trailing run, and stays.
    EXPECT_NE(columnOf(projectId, QStringLiteral("DEMO-0003"), QStringLiteral("body")).indexOf(
                  QStringLiteral("Kind: fix.")), -1);
}

// The user's ruling (2026-09-14): where the run and the column disagree, the
// file shows the RUN today, so stripping it would change what the item says.
TEST(RoadmapRepairTrailers, Inv10ConflictingRunIsSkippedAndListed) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(injectTrailingRun(projectId, QStringLiteral("an older source")));
    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"));

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false, /*stripRuns=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              before.toStdString()) << "a conflicting run was stripped";
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("source")).toStdString(),
              std::string("seed"));
    EXPECT_EQ(resp.value(QStringLiteral("runs_stripped")).toInt(), 0);
    EXPECT_EQ(resp.value(QStringLiteral("strip_skipped")).toInt(), 1)
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(idListed(resp, "strip_skipped_ids", QStringLiteral("DEMO-0004")))
        << QJsonDocument(resp).toJson().toStdString();
}

TEST(RoadmapRepairTrailers, Inv11WithoutStripRunsNoBodyIsWritten) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(injectTrailingRun(projectId, QStringLiteral("seed")));
    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"));

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              before.toStdString());
    EXPECT_FALSE(resp.contains(QStringLiteral("runs_stripped")))
        << "the strip half reported on a run that did not ask for it";
}

TEST(RoadmapRepairTrailers, Inv12StripDryRunPredictsAndSecondRunStripsNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(injectTrailingRun(projectId, QStringLiteral("seed")));
    const QString before =
        columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"));

    RemoteControl rc(nullptr);
    const QJsonObject dry = repair(rc, root, /*dryRun=*/true, /*stripRuns=*/true);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              before.toStdString()) << "dry_run wrote a body";

    const QJsonObject wet = repair(rc, root, false, true);
    ASSERT_TRUE(wet.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(dry.value(QStringLiteral("runs_stripped")).toInt(),
              wet.value(QStringLiteral("runs_stripped")).toInt())
        << "the preview did not predict the run";
    EXPECT_GT(wet.value(QStringLiteral("runs_stripped")).toInt(), 0);

    const QJsonObject again = repair(rc, root, false, true);
    ASSERT_TRUE(again.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(again.value(QStringLiteral("runs_stripped")).toInt(), 0)
        << "the strip is not idempotent";
}

// ----------------------------------------------------------------- INV-13/14 --
// ANTS-4543 — a key the body declares MORE than once, at a line start but not
// in a trailing run, renders every copy. Such a declaration is removed where
// its value equals the column; the render then composes the one line.

TEST(RoadmapRepairTrailers, Inv13RepeatedDeclarationEqualToColumnIsStripped) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"),
                       QStringLiteral("Kind: fix.\n") + QString::fromUtf8(kProse)
                           + QStringLiteral("\nKind: fix.")));

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false, /*stripRuns=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("repeats_stripped")).toInt(), 1)
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              std::string(kProse)) << "the repeated declarations were not removed";
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("kind")).toStdString(),
              std::string("fix"));
    // DEMO-0003 declares each key once mid-body: not a repeat, so untouched.
    EXPECT_NE(columnOf(projectId, QStringLiteral("DEMO-0003"), QStringLiteral("body")).indexOf(
                  QStringLiteral("Kind: fix.")), -1);

    const QJsonObject again = repair(rc, root, false, true);
    EXPECT_EQ(again.value(QStringLiteral("repeats_stripped")).toInt(), 0)
        << "the strip is not idempotent";
}

TEST(RoadmapRepairTrailers, Inv14RepeatWhoseRemovalChangesTheValueIsSkipped) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    // The column is `fix`, the last declaration. Removing it would leave `test`.
    const QString body = QStringLiteral("Kind: test.\n") + QString::fromUtf8(kProse)
                          + QStringLiteral("\nKind: fix.");
    ASSERT_TRUE(damage(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body"), body));

    RemoteControl rc(nullptr);
    const QJsonObject resp = repair(rc, root, false, /*stripRuns=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(columnOf(projectId, QStringLiteral("DEMO-0004"), QStringLiteral("body")).toStdString(),
              body.toStdString()) << "a repeat whose removal changes the value was stripped";
    EXPECT_EQ(resp.value(QStringLiteral("repeats_stripped")).toInt(), 0);
    EXPECT_TRUE(idListed(resp, "strip_skipped_ids", QStringLiteral("DEMO-0004")))
        << QJsonDocument(resp).toJson().toStdString();
}

// ------------------------------------------------------------ INV-15..18 --
// ANTS-4595 — `clear_placeholder_source`. The 2026-04-30 backfill (ANTS-1129)
// wrote `Source: planned.` into bullets that had no provenance, so migration
// stored the lifecycle word as an ASSERTED source. These tests use a fixture of
// their own so the counts the tests above pin do not move.

namespace {

QByteArray placeholderFixture(const char *prefix) {
    const QByteArray p(prefix);
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Placeholder \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n## Work\n\n";
    // Mid-body: migration keeps the line, so the body must lose it too.
    b += "- \xF0\x9F\x93\x8B [" + p + "-0010] **Placeholder kept in the body.**\n"
         "  Kind: fix.\n"
         "  Source: planned.\n"
         "  A closing line, so the run above does not trail the body.\n"
         "\n";
    // Trailing: migration stripped the line, so only the column holds it.
    b += "- \xF0\x9F\x93\x8B [" + p + "-0011] **Placeholder in the column only.**\n"
         "  Some prose before the run.\n"
         "  Kind: fix.\n"
         "  Source: planned.\n"
         "\n";
    // Same shape as -0010; the test gives it a `created` date.
    b += "- \xF0\x9F\x93\x8B [" + p + "-0012] **Dated, so not the backfill.**\n"
         "  Kind: fix.\n"
         "  Source: planned.\n"
         "  A closing line, so the run above does not trail the body.\n"
         "\n";
    // A real source beginning with the word. INV-19 cuts the column back to
    // `planned`, the shape a truncated migration left.
    b += "- \xF0\x9F\x93\x8B [" + p + "-0014] **A real source that starts with the word.**\n"
         "  Kind: fix.\n"
         "  Source: planned for 0.8.\n"
         "  A closing line, so the run above does not trail the body.\n"
         "\n";
    // No Source line: migration applies the default, provenance `defaulted`.
    b += "- \xF0\x9F\x93\x8B [" + p + "-0013] **Defaulted, never asserted.**\n"
         "  Just prose.\n"
         "  Kind: fix.\n"
         "\n";
    return b;
}

QString seedPlaceholder(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                        const char *dir, const char *prefix, const QString &name,
                        qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QString::fromLatin1(dir));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), placeholderFixture(prefix)))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan = RoadmapMigrate::planFrom(*disc, name, name.toLower());
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}

QString sourceProvenanceOf(qint64 projectId, const QString &id) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) { ADD_FAILURE() << "findItem " << id.toStdString(); return QString(); }
    const auto it = store->readItem(*pk, &err);
    if (!it) { ADD_FAILURE() << "readItem " << id.toStdString(); return QString(); }
    return it->provenance.value(QStringLiteral("source")).toString();
}

QJsonObject clearPlaceholders(RemoteControl &rc, const QString &root, bool dryRun) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("repair_trailers");
    req[QStringLiteral("clear_placeholder_source")] = true;
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    return rc.cmdRoadmapLogRepairTrailersForTest(req).object();
}

// Seeds the fixture and checks the preconditions the guard keys on, so a
// fixture that stops producing them fails here rather than passing vacuously.
QString seedAndCheck(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    const QString root = seedPlaceholder(guard, tmp, "ph", "PH", QStringLiteral("Ph"),
                                         projectId);
    if (root.isEmpty()) return root;
    if (!damage(*projectId, QStringLiteral("PH-0012"), QStringLiteral("created"),
                QStringLiteral("2026-04-30")))
        return QString();
    for (const char *id : {"PH-0010", "PH-0011", "PH-0012"}) {
        EXPECT_EQ(columnOf(*projectId, QString::fromLatin1(id), QStringLiteral("source")).toStdString(),
                  std::string("planned")) << id;
        EXPECT_EQ(sourceProvenanceOf(*projectId, QString::fromLatin1(id)).toStdString(),
                  std::string("asserted")) << id;
    }
    EXPECT_EQ(columnOf(*projectId, QStringLiteral("PH-0013"), QStringLiteral("source")).toStdString(),
              std::string("planned"));
    EXPECT_EQ(sourceProvenanceOf(*projectId, QStringLiteral("PH-0013")).toStdString(),
              std::string("defaulted"));
    EXPECT_NE(columnOf(*projectId, QStringLiteral("PH-0010"), QStringLiteral("body"))
                  .indexOf(QStringLiteral("Source: planned.")), -1)
        << "the mid-body line did not survive migration";
    EXPECT_EQ(columnOf(*projectId, QStringLiteral("PH-0011"), QStringLiteral("body"))
                  .indexOf(QStringLiteral("Source:")), -1)
        << "the trailing line was not stripped by migration";
    return root;
}

}  // namespace

TEST(RoadmapRepairTrailers, Inv15AssertedUndatedPlaceholderIsCleared) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedAndCheck(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp = clearPlaceholders(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_EQ(resp.value(QStringLiteral("placeholder_sources_cleared")).toInt(), 2)
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("placeholder_lines_removed")).toInt(), 1);
    EXPECT_TRUE(idListed(resp, "placeholder_cleared_ids", QStringLiteral("PH-0010")));
    EXPECT_TRUE(idListed(resp, "placeholder_cleared_ids", QStringLiteral("PH-0011")));

    EXPECT_TRUE(columnOf(projectId, QStringLiteral("PH-0010"), QStringLiteral("source")).isEmpty());
    EXPECT_TRUE(columnOf(projectId, QStringLiteral("PH-0011"), QStringLiteral("source")).isEmpty());
    const QString body = columnOf(projectId, QStringLiteral("PH-0010"), QStringLiteral("body"));
    EXPECT_EQ(body.indexOf(QStringLiteral("Source:")), -1) << body.toStdString();
    EXPECT_NE(body.indexOf(QStringLiteral("Kind: fix.")), -1) << body.toStdString();
    EXPECT_NE(body.indexOf(QStringLiteral("A closing line")), -1) << body.toStdString();
    // The other trailer columns are not this op's business.
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0010"), QStringLiteral("kind")).toStdString(),
              std::string("fix"));
}

TEST(RoadmapRepairTrailers, Inv16DatedOrDefaultedPlaceholderIsUntouched) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedAndCheck(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString datedBody =
        columnOf(projectId, QStringLiteral("PH-0012"), QStringLiteral("body"));

    RemoteControl rc(nullptr);
    const QJsonObject resp = clearPlaceholders(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0012"), QStringLiteral("source")).toStdString(),
              std::string("planned"));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0012"), QStringLiteral("body")), datedBody);
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0013"), QStringLiteral("source")).toStdString(),
              std::string("planned"));
    EXPECT_EQ(sourceProvenanceOf(projectId, QStringLiteral("PH-0013")).toStdString(),
              std::string("defaulted"));
}

TEST(RoadmapRepairTrailers, Inv17OtherProjectIsUntouched) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 otherId = 0;
    const QString other = seedPlaceholder(guard, tmp, "other", "OT", QStringLiteral("Other"),
                                          &otherId);
    ASSERT_FALSE(other.isEmpty());
    qint64 projectId = 0;
    const QString root = seedAndCheck(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_NE(otherId, projectId);

    RemoteControl rc(nullptr);
    const QJsonObject resp = clearPlaceholders(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("placeholder_sources_cleared")).toInt(), 2);

    for (const char *id : {"OT-0010", "OT-0011"}) {
        EXPECT_EQ(columnOf(otherId, QString::fromLatin1(id), QStringLiteral("source")).toStdString(),
                  std::string("planned")) << id;
    }
    EXPECT_NE(columnOf(otherId, QStringLiteral("OT-0010"), QStringLiteral("body"))
                  .indexOf(QStringLiteral("Source: planned.")), -1);
}

TEST(RoadmapRepairTrailers, Inv18DryRunPredictsAndSecondRunClearsNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedAndCheck(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject dry = clearPlaceholders(rc, root, true);
    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(dry.value(QStringLiteral("placeholder_sources_cleared")).toInt(), 2);
    EXPECT_EQ(dry.value(QStringLiteral("placeholder_lines_removed")).toInt(), 1);
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0010"), QStringLiteral("source")).toStdString(),
              std::string("planned")) << "dry_run wrote the column";

    // Without the flag the op reports none of these fields and clears nothing.
    const QJsonObject plain = repair(rc, root, false);
    ASSERT_TRUE(plain.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(plain.contains(QStringLiteral("placeholder_sources_cleared")));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0010"), QStringLiteral("source")).toStdString(),
              std::string("planned"));

    const QJsonObject real = clearPlaceholders(rc, root, false);
    EXPECT_EQ(real.value(QStringLiteral("placeholder_sources_cleared")).toInt(), 2);
    const QJsonObject again = clearPlaceholders(rc, root, false);
    EXPECT_EQ(again.value(QStringLiteral("placeholder_sources_cleared")).toInt(), 0);
    EXPECT_EQ(again.value(QStringLiteral("placeholder_lines_removed")).toInt(), 0);
}

TEST(RoadmapRepairTrailers, Inv19TruncatedRealSourceIsRepairedNotCleared) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedAndCheck(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(damage(projectId, QStringLiteral("PH-0014"), QStringLiteral("source"),
                       QStringLiteral("planned")));
    ASSERT_EQ(sourceProvenanceOf(projectId, QStringLiteral("PH-0014")).toStdString(),
              std::string("asserted"));

    RemoteControl rc(nullptr);
    const QJsonObject resp = clearPlaceholders(rc, root, false);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(idListed(resp, "placeholder_cleared_ids", QStringLiteral("PH-0014")));
    EXPECT_EQ(columnOf(projectId, QStringLiteral("PH-0014"), QStringLiteral("source")).toStdString(),
              std::string("planned for 0.8"));
}
