// Feature-conformance test for ANTS-4501 slice 3 — the git backfill.
// Contract: docs/specs/ANTS-4501-roadmap-report.md § 2.3.
//
// Covers INV-2 (never overwrites a non-NULL date, so it is re-runnable) and
// INV-3 (writes only dates it observed, and skips the `shipped` COLUMN for an id
// whose current status is not shipped), plus the happy path both rest on and the
// two refusals § 2.3 names.
//
// The fixture builds a REAL git repository with controlled author dates, because
// the thing under test is a history walk: a fake would be asserting against the
// harness. `git log` reads the AUTHOR date (§ 2.3 pins it — the committer date
// moves under every rebase), so the fixture sets both and the assertions key on
// the author one.
//
// The red-run trap this suite is built around: on pre-change source the op does
// not exist, so every case refuses `bad_op_combo` rather than returning wrong
// dates. Each case therefore asserts a VALUE, never merely that nothing moved.

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
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>
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
// REAL store. Safe here only because XdgGuard has redirected XDG_DATA_HOME.
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

bool git(const QString &dir, const QStringList &args, const QString &authorDate = {}) {
    QProcess p;
    p.setWorkingDirectory(dir);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!authorDate.isEmpty()) {
        // Both, because a commit takes its author date from one and its
        // committer date from the other, and leaving the second to the wall
        // clock is what makes a history test flaky at midnight.
        env.insert(QStringLiteral("GIT_AUTHOR_DATE"), authorDate);
        env.insert(QStringLiteral("GIT_COMMITTER_DATE"), authorDate);
    }
    p.setProcessEnvironment(env);
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(3000) || !p.waitForFinished(15000)) {
        ADD_FAILURE() << "git " << args.join(QLatin1Char(' ')).toStdString()
                      << " did not run";
        return false;
    }
    if (p.exitCode() != 0) {
        ADD_FAILURE() << "git " << args.join(QLatin1Char(' ')).toStdString()
                      << " exited " << p.exitCode() << ": "
                      << QString::fromUtf8(p.readAllStandardError()).toStdString();
        return false;
    }
    return true;
}

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

QByteArray bullet(const char *emoji, const char *id, const char *headline) {
    return QByteArray("- ") + emoji + " [" + id + "] **" + headline + "**\n"
           "  Layman: A thing.\n"
           "  Kind: fix.\n"
           "  Source: seed.\n\n";
}

// Revision 1: everything planned.
QByteArray fixtureV1() {
    QByteArray b = "# Demo \xE2\x80\x94 Roadmap\n\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += bullet("\xF0\x9F\x93\x8B", "DEMO-0007", "The item that ships.");
    b += bullet("\xF0\x9F\x93\x8B", "DEMO-0008", "The item that stays open.");
    b += bullet("\xF0\x9F\x93\x8B", "DEMO-0010", "The item that is reopened.");
    return b;
}

// Revision 2: 0007 and 0010 carry the shipped marker.
QByteArray fixtureV2() {
    QByteArray b = "# Demo \xE2\x80\x94 Roadmap\n\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += bullet("\xE2\x9C\x85", "DEMO-0007", "The item that ships.");
    b += bullet("\xF0\x9F\x93\x8B", "DEMO-0008", "The item that stays open.");
    b += bullet("\xE2\x9C\x85", "DEMO-0010", "The item that is reopened.");
    return b;
}

constexpr const char *kAddedDate   = "2026-01-05";
constexpr const char *kShippedDate = "2026-02-10";

// Builds the repo, commits both revisions, migrates the CURRENT file, and adds
// the two rows the INV-3 cases need. Returns the canonical project root.
QString seedRepo(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                 qint64 *projectId, bool makeGitRepo = true) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    QDir().mkpath(rawRoot);
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixtureV1()))
        return QString();
    // The store keys a project on its CANONICAL root (ANTS-3756 INV-8), and
    // /tmp is a symlink on some hosts.
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    if (makeGitRepo) {
        if (!git(root, {QStringLiteral("init"), QStringLiteral("-q")})) return QString();
        if (!git(root, {QStringLiteral("config"), QStringLiteral("user.email"),
                        QStringLiteral("t@example.invalid")})) return QString();
        if (!git(root, {QStringLiteral("config"), QStringLiteral("user.name"),
                        QStringLiteral("T")})) return QString();
        if (!git(root, {QStringLiteral("add"), QStringLiteral("ROADMAP.md")})) return QString();
        if (!git(root, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("seed")},
                 QStringLiteral("%1T10:00:00 +0000").arg(QLatin1String(kAddedDate))))
            return QString();

        if (!writeFile(root + QStringLiteral("/ROADMAP.md"), fixtureV2())) return QString();
        if (!git(root, {QStringLiteral("add"), QStringLiteral("ROADMAP.md")})) return QString();
        if (!git(root, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("ship")},
                 QStringLiteral("%1T10:00:00 +0000").arg(QLatin1String(kShippedDate))))
            return QString();
    } else {
        if (!writeFile(root + QStringLiteral("/ROADMAP.md"), fixtureV2())) return QString();
    }

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

    // INV-3, row 1 — an id the store holds and no revision ever showed. It must
    // stay NULL rather than inherit a boundary commit's date, which afterwards
    // is indistinguishable from a real one.
    RoadmapStore::ItemWrite w;
    w.projectId = out.projectId;
    w.id        = QStringLiteral("DEMO-0099");
    w.status    = QStringLiteral("planned");
    w.headline  = QStringLiteral("Never in git.");
    w.kind      = QStringLiteral("fix");
    w.source    = QStringLiteral("seed");
    w.layman    = QStringLiteral("A thing.");
    const auto sections = store->listSections(out.projectId, &err);
    if (!sections || sections->isEmpty()) {
        ADD_FAILURE() << "no sections after migration";
        return QString();
    }
    w.sectionId = sections->last().sectionId;
    w.position  = 900;
    if (!store->putItem(w, &err)) {
        ADD_FAILURE() << "putItem DEMO-0099: " << err.toStdString();
        return QString();
    }

    // INV-3, row 2 — an id whose HISTORY shows it closed but whose stored status
    // is not shipped: the reopened item. § 2.2 clears `shipped` on the way out,
    // so its column is NULL and INV-2's guard does not protect it, while git
    // still holds the commit where its marker was ✅.
    const auto reopenedPk = store->findItem(out.projectId, QStringLiteral("DEMO-0010"), &err);
    if (!reopenedPk) {
        ADD_FAILURE() << "DEMO-0010 not in the store: " << err.toStdString();
        return QString();
    }
    if (!store->setItemField(*reopenedPk, QStringLiteral("status"),
                             QStringLiteral("planned"), &err)) {
        ADD_FAILURE() << "reopening DEMO-0010: " << err.toStdString();
        return QString();
    }
    return root;
}

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

QJsonObject runBackfill(const QString &root, bool dryRun = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("backfill_dates");
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogBackfillDatesForTest(req).object();
}

#define ASSERT_VERB_OK(resp)                                                    \
    ASSERT_TRUE((resp).value(QStringLiteral("ok")).toBool())                     \
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString()

}  // namespace

// The happy path INV-2 and INV-3 both rest on: an id's `created` is the first
// commit its bullet appears in, its `shipped` the first commit that bullet
// carries ✅. Asserted first, because a backfill that finds nothing satisfies
// half of every clause below by accident.
TEST(RoadmapBackfill, WalkDatesFromTheAuthorDateOfEachCommit) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedRepo(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject resp = runBackfill(root);
    ASSERT_VERB_OK(resp);
    EXPECT_EQ(resp.value(QStringLiteral("revisions_walked")).toInt(), 2);

    const Dates shippedItem = datesOf(projectId, QStringLiteral("DEMO-0007"));
    ASSERT_TRUE(shippedItem.found);
    EXPECT_EQ(shippedItem.created.toStdString(), std::string(kAddedDate));
    EXPECT_EQ(shippedItem.shipped.toStdString(), std::string(kShippedDate));

    // An OPEN item is still dated, on `created` alone. § 2.3 skips the column
    // and not the id, or `added` and `age_open` are unanswerable for exactly
    // the backlog the report is about.
    const Dates openItem = datesOf(projectId, QStringLiteral("DEMO-0008"));
    EXPECT_EQ(openItem.created.toStdString(), std::string(kAddedDate));
    EXPECT_TRUE(openItem.shippedNull);

    // § 5 — `last_modified` is not backfilled at all.
    EXPECT_TRUE(shippedItem.modifiedNull)
        << "the backfill wrote `last_modified`, which § 5 excludes outright";
}

// ------------------------------------------------------------------ INV-2 ---
//
// A non-NULL date is never overwritten — by a stamp, by a hand correction, or by
// an earlier run. That is what makes a second run the same operation as the
// first rather than a different one.
TEST(RoadmapBackfill, Inv2NeverOverwritesAndIsReRunnable) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedRepo(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_TRUE(store != nullptr);
        QString err;
        const auto pk = store->findItem(projectId, QStringLiteral("DEMO-0007"), &err);
        ASSERT_TRUE(pk.has_value()) << err.toStdString();
        ASSERT_TRUE(store->setItemField(*pk, QStringLiteral("shipped"),
                                        QStringLiteral("2026-01-01"), &err))
            << err.toStdString();
    }

    ASSERT_VERB_OK(runBackfill(root));
    const Dates first = datesOf(projectId, QStringLiteral("DEMO-0007"));
    EXPECT_EQ(first.shipped.toStdString(), std::string("2026-01-01"))
        << "the walk would have derived " << kShippedDate
        << " and overwrote a value that was already set";
    EXPECT_EQ(first.created.toStdString(), std::string(kAddedDate))
        << "the NULL column beside it should still have been filled";

    // Two more runs, because unbounded drift is the failure and one repeat
    // cannot show a trend.
    for (int i = 0; i < 2; ++i) {
        ASSERT_VERB_OK(runBackfill(root));
        const Dates again = datesOf(projectId, QStringLiteral("DEMO-0007"));
        EXPECT_EQ(again.shipped.toStdString(), first.shipped.toStdString());
        EXPECT_EQ(again.created.toStdString(), first.created.toStdString());
    }
}

// ------------------------------------------------------------------ INV-3 ---
//
// Two rows, and the second is the one a build passes by accident: the first
// alone is satisfied by any backfill that simply finds nothing.
TEST(RoadmapBackfill, Inv3WritesOnlyDatesItObserved) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedRepo(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject resp = runBackfill(root);
    ASSERT_VERB_OK(resp);

    // Row 1 — an id no revision showed keeps all three columns NULL, and is
    // COUNTED rather than silently dropped.
    const Dates unseen = datesOf(projectId, QStringLiteral("DEMO-0099"));
    ASSERT_TRUE(unseen.found);
    EXPECT_TRUE(unseen.createdNull)  << "an unmatched id inherited a date";
    EXPECT_TRUE(unseen.modifiedNull);
    EXPECT_TRUE(unseen.shippedNull);
    EXPECT_GE(resp.value(QStringLiteral("undated_count")).toInt(), 1)
        << "the id it could not date is not reported, so a caller cannot tell "
           "an empty answer from a complete one";

    // Row 2 — history shows DEMO-0010 closed; the store says it is planned. Its
    // `created` is written (the walk observed the bullet) and its `shipped` is
    // NOT, or every reopened item is silently re-closed on every run.
    const Dates reopened = datesOf(projectId, QStringLiteral("DEMO-0010"));
    ASSERT_TRUE(reopened.found);
    EXPECT_EQ(reopened.created.toStdString(), std::string(kAddedDate))
        << "skipping the whole id leaves every reopened item undated";
    EXPECT_TRUE(reopened.shippedNull)
        << "a reopened item was re-closed from git — its ✅ is still in the "
           "history and nothing but this rule stands between them";
}

// dry_run reports the counts the real run would write, and writes nothing.
TEST(RoadmapBackfill, DryRunReportsTheRealRunAndWritesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedRepo(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject preview = runBackfill(root, /*dryRun=*/true);
    ASSERT_VERB_OK(preview);
    EXPECT_TRUE(preview.value(QStringLiteral("dry_run")).toBool());
    EXPECT_TRUE(datesOf(projectId, QStringLiteral("DEMO-0007")).createdNull)
        << "dry_run wrote to the store";

    const QJsonObject real = runBackfill(root);
    ASSERT_VERB_OK(real);
    EXPECT_EQ(real.value(QStringLiteral("created_written")).toInt(),
              preview.value(QStringLiteral("created_written")).toInt());
    EXPECT_EQ(real.value(QStringLiteral("shipped_written")).toInt(),
              preview.value(QStringLiteral("shipped_written")).toInt());
    EXPECT_GT(real.value(QStringLiteral("created_written")).toInt(), 0)
        << "a preview matching a real run that wrote nothing proves nothing";
}

// § 2.3's two refusals.
TEST(RoadmapBackfill, RefusesOutsideAGitRepo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = -1;
    const QString root = seedRepo(guard, tmp, &projectId, /*makeGitRepo=*/false);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject resp = runBackfill(root);
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("not_a_git_repo"));
}

TEST(RoadmapBackfill, RefusesAProjectTheStoreDoesNotHold) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("unmigrated"));
    QDir().mkpath(rawRoot);
    ASSERT_TRUE(writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixtureV2()));
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    const QJsonObject resp = runBackfill(root);
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString().toStdString(),
              std::string("project_not_registered"))
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
}

// A dry run against THIS repository's real history and a COPY of the real
// store. DISABLED, following roadmap_migrate_load's CorpusArchiveRun: it needs
// the developer's own data and takes seconds, so it is opt-in
// (--gtest_also_run_disabled_tests).
//
// The store is COPIED into the suite's QStandardPaths sandbox rather than
// opened where it lives. This binary runs with test mode on, so
// RoadmapStore::defaultPath() already resolves under /tmp and the real store is
// unreachable from here by design (roadmap_store_sandbox) — and a corpus case
// that reached around that sandbox would be pointing the write path at the
// developer's own data to save a file copy. The git history is read from the
// real repository, which the walk only ever reads.
//
// It exists because a synthetic two-commit fixture cannot say whether the walk
// fits the MCP bridge's 60 s budget, and that is the one question that decided
// the diff-based design.
TEST(RoadmapBackfill, DISABLED_CorpusDryRun) {
    const QString root =
        QFileInfo(QString::fromUtf8(ANTS_SOURCE_DIR)).canonicalFilePath();
    ASSERT_FALSE(root.isEmpty());

    const QString liveStore =
        QDir::homePath() + QStringLiteral("/.local/share/ants-terminal/roadmap.sqlite");
    if (!QFileInfo::exists(liveStore))
        GTEST_SKIP() << "no store at " << liveStore.toStdString();
    const QString sandbox = RoadmapStore::defaultPath();
    QDir().mkpath(QFileInfo(sandbox).path());
    QFile::remove(sandbox);
    ASSERT_TRUE(QFile::copy(liveStore, sandbox))
        << "could not copy the store into " << sandbox.toStdString();
    std::printf("  root=%s\n  store=%s (copy)\n", root.toUtf8().constData(),
                sandbox.toUtf8().constData());

    const QJsonObject resp = runBackfill(root, /*dryRun=*/true);
    ASSERT_VERB_OK(resp);
    for (const QString &k : {QStringLiteral("revisions_walked"), QStringLiteral("walk_ms"),
                             QStringLiteral("items"), QStringLiteral("created_written"),
                             QStringLiteral("shipped_written"),
                             QStringLiteral("undated_count")})
        std::printf("  %-18s %s\n", k.toUtf8().constData(),
                    QString::number(resp.value(k).toDouble()).toUtf8().constData());
    EXPECT_LT(resp.value(QStringLiteral("walk_ms")).toDouble(), 60000.0)
        << "the walk no longer fits the MCP bridge's 60 s budget (ANTS-3444)";
}
