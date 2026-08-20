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

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

QJsonObject repair(RemoteControl &rc, const QString &root, bool dryRun) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("repair_trailers");
    if (dryRun) req[QStringLiteral("dry_run")] = true;
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
    return QString();
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
