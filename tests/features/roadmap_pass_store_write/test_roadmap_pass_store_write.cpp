// ANTS-5334 — writes on a store-served pass-headings roadmap go through the
// store. Contract: tests/features/roadmap_pass_store_write/spec.md
//
// Behavioural, through the roadmap_log verbs: each case migrates a small
// `#### Pass N.M` roadmap into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox, as roadmap_write_half does), drives a `*ForTest`
// entry point, and re-opens the store to assert what landed.

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
#include <QTemporaryDir>

#include <memory>

namespace {

const char *kSeed =
    "# Demo — Roadmap\n"
    "\n"
    "## Phase 4\n"
    "\n"
    "#### Pass 43.5 Harden the parser against a truncated frame.\n"
    "- **Status**: done\n"
    "  The frame length is validated before the body is read.\n"
    "\n"
    "#### Pass 43.5.B Follow-up: the same check on the reply path.\n"
    "- **Status**: todo\n"
    "\n"
    "## Phase 5\n"
    "\n"
    "#### Pass 44.1 Retire the legacy transport.\n"
    "- **Status**: in-progress\n"
    "  Blocked on the 43.5 follow-up.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME, which every case redirects first.
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

// Write the seed at <tmp>/proj and return its canonical root. With `migrate`,
// also load it into the sandboxed store (Bulk, closed before the verb runs).
QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp, bool migrate,
             qint64 *projectId = nullptr) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), QByteArray(kSeed)))
        return QString();
    QString root = QFileInfo(rawRoot).canonicalFilePath();
    if (!migrate) return root;

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-25T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    if (projectId) *projectId = out.projectId;
    return root;
}

std::optional<RoadmapStore::ItemWrite> itemOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return std::nullopt;
    return store->readItem(*pk, &err);
}

QJsonObject req(const QString &root, const QString &op) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = op;
    return r;
}

}  // namespace

// INV-1
TEST(RoadmapPassStoreWrite, Inv1FlipWritesThroughTheStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, /*migrate=*/true, &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(), QStringLiteral("pass-headings"));

    const auto item = itemOf(QStringLiteral("PASS-43-5-B"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, QStringLiteral("shipped"))
        << "the flip did not reach the store";
    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(QStringLiteral(
        "#### Pass 43.5.B Follow-up: the same check on the reply path.\n- **Status**: done")))
        << file.toStdString();
}

// INV-2
TEST(RoadmapPassStoreWrite, Inv2DryRunPreviewsWithoutWriting) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(root + QStringLiteral("/ROADMAP.md"));

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    r[QStringLiteral("dry_run")]   = true;
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("would_write")).toArray().isEmpty())
        << "a store-path preview names the file it would write";
    EXPECT_EQ(itemOf(QStringLiteral("PASS-43-5-B"), projectId)->status,
              QStringLiteral("planned"));
    EXPECT_EQ(readAll(root + QStringLiteral("/ROADMAP.md")), before);
}

// INV-3
TEST(RoadmapPassStoreWrite, Inv3AnnotateAppendsToTheStoredBody) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("PASS-44-1");
    r[QStringLiteral("note")] = QStringLiteral("Unblocked: the follow-up landed.");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("PASS-44-1"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->body.contains(QStringLiteral("Unblocked: the follow-up landed.")))
        << item->body.toStdString();
    EXPECT_EQ(item->status, QStringLiteral("in-progress"));
}

// INV-4
TEST(RoadmapPassStoreWrite, Inv4OpsWithNoStoreRouteRefuse) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(root + QStringLiteral("/ROADMAP.md"));
    const QString bodyBefore = itemOf(QStringLiteral("PASS-44-1"), projectId)->body;

    QJsonObject append = req(root, QStringLiteral("append"));
    append[QStringLiteral("section")]  = QStringLiteral("phase-5");
    append[QStringLiteral("status")]   = QStringLiteral("planned");
    append[QStringLiteral("pass")]     = QStringLiteral("44.2");
    append[QStringLiteral("headline")] = QStringLiteral("A new pass.");
    append[QStringLiteral("kind")]     = QStringLiteral("implement");
    append[QStringLiteral("source")]   = QStringLiteral("test");

    QJsonObject appendBatch = req(root, QStringLiteral("append_batch"));
    appendBatch[QStringLiteral("section")] = QStringLiteral("phase-5");
    QJsonObject b;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("pass")]     = QStringLiteral("44.2");
    b[QStringLiteral("headline")] = QStringLiteral("A new pass.");
    b[QStringLiteral("kind")]     = QStringLiteral("implement");
    b[QStringLiteral("source")]   = QStringLiteral("test");
    appendBatch[QStringLiteral("bullets")] = QJsonArray{b};

    QJsonObject loc;
    loc[QStringLiteral("id")]   = QStringLiteral("PASS-43-5-B");
    loc[QStringLiteral("note")] = QStringLiteral("A batch note.");
    QJsonObject flipBatch = req(root, QStringLiteral("flip_batch"));
    flipBatch[QStringLiteral("to_status")] = QStringLiteral("shipped");
    flipBatch[QStringLiteral("locators")]  = QJsonArray{loc};
    QJsonObject annotateBatch = req(root, QStringLiteral("annotate_batch"));
    QJsonObject aloc = loc;
    aloc[QStringLiteral("id")] = QStringLiteral("PASS-44-1");
    annotateBatch[QStringLiteral("locators")] = QJsonArray{aloc};

    RemoteControl rc(nullptr);
    const struct { const char *op; QJsonObject resp; } cases[] = {
        {"append",         rc.cmdRoadmapLogAppendForTest(append).object()},
        {"append_batch",   rc.cmdRoadmapLogAppendBatchForTest(appendBatch).object()},
        {"flip_batch",     rc.cmdRoadmapLogFlipBatchForTest(flipBatch).object()},
        {"annotate_batch", rc.cmdRoadmapLogFlipBatchForTest(annotateBatch).object()},
    };
    for (const auto &c : cases) {
        EXPECT_FALSE(c.resp.value(QStringLiteral("ok")).toBool()) << c.op;
        EXPECT_EQ(c.resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("unsupported_format"))
            << c.op << ": " << QJsonDocument(c.resp).toJson().toStdString();
    }
    EXPECT_EQ(readAll(root + QStringLiteral("/ROADMAP.md")), before)
        << "a refused op wrote the file behind the store";
    EXPECT_FALSE(itemOf(QStringLiteral("PASS-44-2"), projectId).has_value());
    EXPECT_EQ(itemOf(QStringLiteral("PASS-43-5-B"), projectId)->status,
              QStringLiteral("planned"));
    EXPECT_EQ(itemOf(QStringLiteral("PASS-44-1"), projectId)->body, bodyBefore);
}

// INV-5 — boundary: holds before and after the fix.
TEST(RoadmapPassStoreWrite, Inv5UnmigratedPassProjectStillWritesTheFile) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, /*migrate=*/false);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(), QStringLiteral("pass-headings"));
    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(QStringLiteral(
        "#### Pass 43.5.B Follow-up: the same check on the reply path.\n- **Status**: done")))
        << file.toStdString();
}
