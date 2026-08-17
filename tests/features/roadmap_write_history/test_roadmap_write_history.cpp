// ANTS-3822 — consumer writes append a history row. Contract: spec.md beside
// this file; design: docs/specs/ANTS-3822-consumer-write-history.md.
//
// Harness copied in shape from tests/features/roadmap_divergence_guard/: XDG
// redirected into a QTemporaryDir, a small fixture migrated into a store at
// RoadmapStore::defaultPath(), then the real verbs driven and the history table
// read back by raw SELECT.
//
// The red-run trap this suite is built around: INV-1, INV-2 and INV-6 all fail
// against pre-fix code by returning ZERO rows, and zero is equally what a
// fixture that never migrated returns. So every row assertion is preceded by a
// check that the MIGRATION's own rows are visible — see spec.md.

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
#include <QSqlDatabase>
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
// REAL store under XDG_DATA_HOME. `Access` is the THIRD parameter, after the
// history cap — which is what makes the cap cheap to inject for INV-5.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access,
                                        qint64 cap = RoadmapStore::kDefaultHistoryCapBytes) {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath(), cap, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
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
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

// Three open items, each with a Layman: line so the render's gate is clear and
// the only refusal a case can hit is the one under test. Two are needed for
// INV-2's batch leg.
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
         "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
         "  Layman: A thing.\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n"
         "- \xF0\x9F\x93\x8B [DEMO-0008] **A second open item.**\n"
         "  Layman: Another thing.\n"
         "  Kind: fix.\n"
         "  Source: seed.\n"
         "\n"
         "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
         "  Layman: A third thing.\n"
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

    // Seed ONE sentinel history row, on the item no case touches.
    //
    // This exists to make the fixture guard below meaningful, and it is here
    // rather than relying on the migration because a FIRST migration writes no
    // history at all: Loader::recordHistory() is reached only from
    // applyPlanFields(), which compares a stored value against a planned one and
    // so runs only for items that already exist. Every item here is created by
    // putItem() on this run. (The live store's rows come from RE-migrations.)
    //
    // Measured the hard way: the guard fired on its own false premise before any
    // of these cases could assert anything — which is the guard working.
    QString hErr;
    const auto seedPk = store->findItem(out.projectId, QStringLiteral("DEMO-0003"), &hErr);
    if (!seedPk) {
        ADD_FAILURE() << "seed item DEMO-0003 not in the store: " << hErr.toStdString();
        return QString();
    }
    if (!store->appendHistory(*seedPk, QStringLiteral("2026-08-14T10:00:00Z"), 0,
                              QStringLiteral("status"), QStringLiteral("planned"),
                              QStringLiteral("shipped"), &hErr)) {
        ADD_FAILURE() << "seeding a history row: " << hErr.toStdString();
        return QString();
    }
    return root;
}

// --- reading history back, by raw SELECT (see spec.md § Test shape) ---------

struct HistRow {
    QString changedAt, field, oldValue, newValue;
    int     seq = 0;
    bool    oldIsNull = false, newIsNull = false;
};

QVector<HistRow> historyOf(qint64 itemPk) {
    QVector<HistRow> out;
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return out;
    QSqlQuery q(store->db());
    q.prepare(QStringLiteral(
        "SELECT changed_at, seq, field, old_value, new_value FROM history "
        "WHERE item_pk = ? ORDER BY changed_at, seq"));
    q.addBindValue(itemPk);
    if (!q.exec()) {
        ADD_FAILURE() << "history SELECT: " << q.lastError().text().toStdString();
        return out;
    }
    while (q.next()) {
        HistRow r;
        r.changedAt = q.value(0).toString();
        r.seq       = q.value(1).toInt();
        r.field     = q.value(2).toString();
        r.oldIsNull = q.value(3).isNull();
        r.newIsNull = q.value(4).isNull();
        r.oldValue  = q.value(3).toString();
        r.newValue  = q.value(4).toString();
        out.push_back(r);
    }
    return out;
}

int historyRowCount() {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return -1;
    QSqlQuery q(store->db());
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM history")) || !q.next())
        return -1;
    return q.value(0).toInt();
}

qint64 historySumBytes() {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return -1;
    QSqlQuery q(store->db());
    if (!q.exec(QStringLiteral(
            "SELECT COALESCE(SUM(length(field) + length(coalesce(old_value,'')) "
            "+ length(coalesce(new_value,''))), 0) FROM history")) || !q.next())
        return -1;
    return q.value(0).toLongLong();
}

qint64 pkOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return 0;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    return pk.value_or(0);
}

QString statusOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return QString();
    const auto item = store->readItem(*pk, &err);
    return item ? item->status : QString();
}

// --- driving the verbs ------------------------------------------------------

// `historyCap` < 0 means production's default.
//
// It is a parameter rather than something the caller sets on its own
// RemoteControl, because this function constructs the instance that does the
// write. Setting the cap on a DIFFERENT instance is silent: the store is opened
// per instance, so the flip ran at the 250 MiB default and INV-5 asserted
// against an implementation it had never put under the cap. Measured — that is
// exactly how it failed first.
QJsonObject runFlip(const QString &root, const QString &id, bool dryRun = false,
                    qint64 historyCap = -1) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = id;
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    RemoteControl rc(nullptr);
    if (historyCap >= 0) rc.setRoadmapHistoryCapForTest(historyCap);
    return rc.cmdRoadmapLogFlipForTest(req).object();
}

// The guard every row assertion needs. seedMigrated() writes one sentinel row;
// if this cannot see it, the XDG redirect, the store path or the SELECT is
// broken, and a later "zero consumer rows" would be meaningless rather than a
// failure. spec.md § Why the red run needs proving first.
#define ASSERT_FIXTURE_HISTORY_VISIBLE()                                        \
    do {                                                                        \
        const int seeded = historyRowCount();                                   \
        ASSERT_GT(seeded, 0)                                                     \
            << "the seeded sentinel history row is invisible, so the fixture's "  \
               "store path or SELECT is broken — every 'zero rows' assertion "    \
               "below would pass for the wrong reason";                          \
    } while (0)

}  // namespace

// ------------------------------------------------------------------ INV-1 ---

TEST(RoadmapWriteHistory, Inv1FlipRecordsTheStatusChange) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_FIXTURE_HISTORY_VISIBLE();

    const qint64 pk = pkOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_GT(pk, 0);
    const auto before = historyOf(pk);

    const QJsonObject resp = runFlip(root, QStringLiteral("DEMO-0007"));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("shipped"));

    const auto after = historyOf(pk);
    ASSERT_GT(after.size(), before.size())
        << "the flip wrote no history row at all — the audit trail still stops "
           "at migration time, which is the whole of ANTS-3822";

    bool sawStatus = false;
    for (int i = before.size(); i < after.size(); ++i) {
        if (after[i].field == QStringLiteral("status")) {
            sawStatus = true;
            EXPECT_EQ(after[i].newValue, QStringLiteral("shipped"));
            EXPECT_EQ(after[i].oldValue, QStringLiteral("planned"))
                << "old_value must be the value the column HELD, not the new one";
        }
    }
    EXPECT_TRUE(sawStatus) << "a status flip recorded no `status` row";
}

// ------------------------------------------------------------------ INV-2 ---

TEST(RoadmapWriteHistory, Inv2OneStampAndContiguousSeqPerItem) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_FIXTURE_HISTORY_VISIBLE();

    const qint64 pk = pkOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_GT(pk, 0);
    const auto before = historyOf(pk);

    ASSERT_TRUE(runFlip(root, QStringLiteral("DEMO-0007"))
                    .value(QStringLiteral("ok")).toBool());

    const auto after = historyOf(pk);
    ASSERT_GT(after.size(), before.size());

    // Rows this op added share ONE changed_at, and their seq set is contiguous.
    QSet<QString> stamps;
    QVector<int> seqs;
    for (int i = before.size(); i < after.size(); ++i) {
        stamps.insert(after[i].changedAt);
        seqs.push_back(after[i].seq);
    }
    EXPECT_EQ(stamps.size(), 1)
        << "one op must be one revision: its rows share a single changed_at";
    std::sort(seqs.begin(), seqs.end());
    for (int i = 1; i < seqs.size(); ++i)
        EXPECT_EQ(seqs[i], seqs[i - 1] + 1) << "seq must be contiguous, no gaps";

    // The migration stamped 2026-08-14T10:00:00Z; this op's stamp is its own.
    EXPECT_NE(*stamps.cbegin(), QStringLiteral("2026-08-14T10:00:00Z"))
        << "the consumer write reused the migration's stamp";
}

// The leg that separates a per-item cursor from one op-wide counter. Two items,
// NEITHER pre-seeded at the op's stamp — deliberately, because changed_at is
// second-resolution and the op computes its own, so a seeded row only shares it
// if the clock does not tick between seeding and the call.
TEST(RoadmapWriteHistory, Inv2BatchNumbersEachItemFromItsOwnBase) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_FIXTURE_HISTORY_VISIBLE();

    const qint64 pkA = pkOf(QStringLiteral("DEMO-0007"), projectId);
    const qint64 pkB = pkOf(QStringLiteral("DEMO-0008"), projectId);
    ASSERT_GT(pkA, 0);
    ASSERT_GT(pkB, 0);
    const int beforeA = historyOf(pkA).size();
    const int beforeB = historyOf(pkB).size();

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    QJsonArray locs;
    for (const char *id : {"DEMO-0007", "DEMO-0008"}) {
        QJsonObject l;
        l[QStringLiteral("id")] = QString::fromLatin1(id);
        locs.append(l);
    }
    req[QStringLiteral("locators")] = locs;
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipBatchForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    const auto afterA = historyOf(pkA);
    const auto afterB = historyOf(pkB);
    ASSERT_GT(afterA.size(), beforeA);
    ASSERT_GT(afterB.size(), beforeB);

    // Both items' new rows must start from THEIR OWN base. An op-wide counter
    // gives the second item the first item's numbering continued.
    const int firstNewSeqA = afterA[beforeA].seq;
    const int firstNewSeqB = afterB[beforeB].seq;
    EXPECT_EQ(firstNewSeqA, firstNewSeqB)
        << "seq is scoped per (item_pk, changed_at), so two items with the same "
           "prior history must start at the same number — got A=" << firstNewSeqA
        << " B=" << firstNewSeqB << ", which is one counter shared across the op";
}

// ------------------------------------------------------------------ INV-3 ---

TEST(RoadmapWriteHistory, Inv3DryRunLeavesHistoryByteIdentical) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_FIXTURE_HISTORY_VISIBLE();

    const int  rowsBefore  = historyRowCount();
    const qint64 bytesBefore = historySumBytes();
    ASSERT_GT(rowsBefore, 0);

    const QJsonObject resp = runFlip(root, QStringLiteral("DEMO-0007"), /*dryRun=*/true);
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    EXPECT_EQ(historyRowCount(), rowsBefore)
        << "a dry run wrote history rows — it must commit nothing on either the "
           "store or the file";
    EXPECT_EQ(historySumBytes(), bytesBefore);
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("planned"))
        << "a dry run also must not change the item";
}

// ------------------------------------------------------------------ INV-4 ---

// Store level, deliberately: commitAndRender()'s gate is project-scoped, so a
// two-phase recipe through the verb cannot reach its second phase.
TEST(RoadmapWriteHistory, Inv4RollbackLeavesNoRows) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const qint64 pk = pkOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_GT(pk, 0);
    const int before = historyOf(pk).size();

    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        ASSERT_TRUE(store->begin(&err)) << err.toStdString();
        ASSERT_TRUE(store->appendHistory(pk, QStringLiteral("2026-08-17T12:00:00Z"),
                                         0, QStringLiteral("status"),
                                         QStringLiteral("planned"),
                                         QStringLiteral("shipped"), &err))
            << err.toStdString();
        ASSERT_TRUE(store->rollback(&err)) << err.toStdString();
    }

    EXPECT_EQ(historyOf(pk).size(), before)
        << "a rolled-back transaction left a history row behind";
}

// ------------------------------------------------------------------ INV-5 ---

// One leg at verb level, which ANTS-3822 § 2.3.2's injectable cap exists to make
// possible: the cap is set below the verb, the envelope is built above it, and
// all three assertions have to hold at once.
TEST(RoadmapWriteHistory, Inv5AtTheCapTheWriteStandsAndTheEnvelopeSaysSo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const qint64 pk = pkOf(QStringLiteral("DEMO-0007"), projectId);
    ASSERT_GT(pk, 0);
    const int before = historyOf(pk).size();

    // A cap of 1 byte: the seeded row already exceeds it, so the op's batch
    // cannot fit and every row is skipped. Passed INTO runFlip, because it is
    // that call which builds the RemoteControl whose store carries the cap.
    const QJsonObject resp =
        runFlip(root, QStringLiteral("DEMO-0007"), /*dryRun=*/false, /*historyCap=*/1);

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "a full history table must NOT refuse the roadmap write — the audit "
           "trail taking the roadmap down with it is the opposite of the point: "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("shipped"))
        << "the item write must stand even when its revision cannot be recorded";
    EXPECT_EQ(historyOf(pk).size(), before)
        << "at the cap nothing is written — not even part of the revision";
    ASSERT_TRUE(resp.contains(QStringLiteral("history_note")))
        << "the skip was SILENT. INV-14's rule is 'fails AND reports', and an "
           "envelope with no history_note is indistinguishable from a write that "
           "recorded its revision";
    EXPECT_TRUE(resp.value(QStringLiteral("history_note")).toString()
                    .contains(QStringLiteral("row(s) not recorded")))
        << "history_note: "
        << resp.value(QStringLiteral("history_note")).toString().toStdString();
}

// ------------------------------------------------------------------ INV-6 ---

TEST(RoadmapWriteHistory, Inv6AppendWritesNoHistory) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_FIXTURE_HISTORY_VISIBLE();

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("append");
    req[QStringLiteral("section")]    = QStringLiteral("work");
    req[QStringLiteral("status")]     = QStringLiteral("planned");
    req[QStringLiteral("headline")]   = QStringLiteral("A newly created item.");
    req[QStringLiteral("kind")]       = QStringLiteral("chore");
    req[QStringLiteral("source")]     = QStringLiteral("test");
    req[QStringLiteral("layman")]     = QStringLiteral("Something new.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();

    // `id`, singular — op:append's envelope key. (`ids` is append_batch's.)
    const QString newId = resp.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(newId.isEmpty()) << "append returned no id: "
        << QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString();
    const qint64 pk = pkOf(newId, projectId);
    ASSERT_GT(pk, 0) << "the appended item is not in the store";

    EXPECT_TRUE(historyOf(pk).isEmpty())
        << "a CREATION recorded history. Its old value does not exist — a row "
           "claiming a transition from '' invents a state the item was never in, "
           "and the item's own row is the record that it was created";
}
