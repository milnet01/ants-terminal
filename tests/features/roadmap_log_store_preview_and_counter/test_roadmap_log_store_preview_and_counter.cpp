// Feature-conformance test for ANTS-4634 and ANTS-4635 — roadmap_log's
// STORE-backed write branch.
// Contract: tests/features/roadmap_log_store_preview_and_counter/spec.md
//
// Behavioural through the *ForTest entry points against a real migrated
// fixture. XDG_DATA_HOME is redirected per case so RoadmapStore::defaultPath()
// — which the handlers resolve internally, and no argument overrides — lands in
// a QTemporaryDir instead of the developer's REAL machine-global store.
//
// The fixture must be migrated and ants-v1: RoadmapSource::migratedProject()
// serves the store only for that dialect, so a plain markdown fixture would
// silently exercise the branch that already passes and prove nothing.

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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

// Every open item carries a Layman line: the render's own gate refuses a write
// that touches an open item without one, so a fixture missing it fails for a
// reason that has nothing to do with this suite.
QByteArray roadmapText() {
    return "<!-- ants-roadmap-format: 1 -->\n"
           "\n"
           "# Demo — Roadmap\n"
           "\n"
           "## Work\n"
           "\n"
           "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
           "  Layman: A thing.\n"
           "  Kind: implement.\n"
           "  Source: fixture.\n";
}

class XdgRedirect {
public:
    explicit XdgRedirect(const QString &dir)
        : m_had(qEnvironmentVariableIsSet("XDG_DATA_HOME")),
          m_prior(m_had ? qgetenv("XDG_DATA_HOME") : QByteArray()) {
        qputenv("XDG_DATA_HOME", dir.toLocal8Bit());
    }
    ~XdgRedirect() {
        if (m_had) qputenv("XDG_DATA_HOME", m_prior);
        else       qunsetenv("XDG_DATA_HOME");
    }
    XdgRedirect(const XdgRedirect &) = delete;
    XdgRedirect &operator=(const XdgRedirect &) = delete;
private:
    bool m_had;
    QByteArray m_prior;
};

// Bulk, because RoadmapMigrateLoad::load() refuses an Interactive connection
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
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-24T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

// A migrated fixture plus the counter cache a real project carries. The
// counter must EXIST: the reconciliation deliberately does not create one, so
// a fixture without it would pass INV-5 vacuously.
struct Fixture {
    QTemporaryDir xdg;
    QTemporaryDir proj;
    QString root;

    bool setUp(qint64 counterSeed) {
        if (!xdg.isValid() || !proj.isValid()) return false;
        root = proj.path();
        if (!writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapText()))
            return false;
        if (!writeFile(root + QStringLiteral("/.roadmap-counter"),
                       (QString::number(counterSeed) + QChar('\n')).toUtf8()))
            return false;
        return true;
    }
};

qint64 readCounter(const QString &root) {
    QFile f(root + QStringLiteral("/.roadmap-counter"));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}

QJsonObject appendReq(const QString &root, const QString &headline) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("append");
    r[QStringLiteral("section")]    = QStringLiteral("work");
    r[QStringLiteral("status")]     = QStringLiteral("planned");
    r[QStringLiteral("headline")]   = headline;
    r[QStringLiteral("kind")]       = QStringLiteral("implement");
    r[QStringLiteral("source")]     = QStringLiteral("test");
    r[QStringLiteral("layman")]     = QStringLiteral("A plain sentence.");
    return r;
}

QJsonObject batchBullet(const QString &headline) {
    QJsonObject b;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("headline")] = headline;
    b[QStringLiteral("kind")]     = QStringLiteral("implement");
    b[QStringLiteral("source")]   = QStringLiteral("test");
    b[QStringLiteral("layman")]   = QStringLiteral("A plain sentence.");
    return b;
}

QJsonObject batchReq(const QString &root, const QStringList &headlines) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("append_batch");
    r[QStringLiteral("section")]    = QStringLiteral("work");
    QJsonArray bs;
    for (const QString &h : headlines) bs.append(batchBullet(h));
    r[QStringLiteral("bullets")] = bs;
    return r;
}

// Guards the whole suite: if the handler answered from markdown, every
// assertion below would be testing the branch that already passed. The store
// branch is the one that emits items_rendered.
void assertStoreBranch(const QJsonObject &env, const char *what) {
    ASSERT_TRUE(env.contains(QStringLiteral("items_rendered")))
        << what << " did not take the STORE branch — this suite would be "
                   "asserting the markdown path, which is already covered\n"
        << QJsonDocument(env).toJson().toStdString();
}

}  // namespace

// ------------------------------------------------------------------ INV-1 --
TEST(RoadmapLogStorePreviewAndCounter, AppendDryRunReportsWouldBeIdNotId) {
    Fixture fx;
    ASSERT_TRUE(fx.setUp(7));
    XdgRedirect redirect(fx.xdg.path());
    ASSERT_TRUE(migrateDefaultStore(fx.root));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(fx.root, QStringLiteral("Preview only."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject env = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(env[QStringLiteral("ok")].toBool())
        << QJsonDocument(env).toJson().toStdString();
    assertStoreBranch(env, "append dry_run");

    EXPECT_TRUE(env.contains(QStringLiteral("would_be_id")))
        << "INV-1: a preview must report the would-be id under `would_be_id`\n"
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_FALSE(env.contains(QStringLiteral("id")))
        << "INV-1: the real write's key must not appear on a preview — a "
           "caller reading one field would read it as a reservation\n"
        << QJsonDocument(env).toJson().toStdString();
}

// ------------------------------------------------------------------ INV-2 --
TEST(RoadmapLogStorePreviewAndCounter, AppendRealWriteStillReportsId) {
    Fixture fx;
    ASSERT_TRUE(fx.setUp(7));
    XdgRedirect redirect(fx.xdg.path());
    ASSERT_TRUE(migrateDefaultStore(fx.root));

    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdRoadmapLogAppendForTest(
        appendReq(fx.root, QStringLiteral("A real item."))).object();
    ASSERT_TRUE(env[QStringLiteral("ok")].toBool())
        << QJsonDocument(env).toJson().toStdString();
    assertStoreBranch(env, "append");

    EXPECT_FALSE(env[QStringLiteral("id")].toString().isEmpty())
        << "INV-2: a real append still allocates under `id`";
    EXPECT_FALSE(env.contains(QStringLiteral("would_be_id")))
        << "INV-2: the rename is preview-only — the two envelopes must stay "
           "distinguishable by the key that carries the id";
}

// ------------------------------------------------------------------ INV-3 --
TEST(RoadmapLogStorePreviewAndCounter, BatchDryRunReportsWouldBeIdsNotIds) {
    Fixture fx;
    ASSERT_TRUE(fx.setUp(7));
    XdgRedirect redirect(fx.xdg.path());
    ASSERT_TRUE(migrateDefaultStore(fx.root));

    RemoteControl rc(nullptr);
    QJsonObject req = batchReq(fx.root, {QStringLiteral("First preview."),
                                         QStringLiteral("Second preview.")});
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject env = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(env[QStringLiteral("ok")].toBool())
        << QJsonDocument(env).toJson().toStdString();
    assertStoreBranch(env, "append_batch dry_run");

    EXPECT_EQ(env[QStringLiteral("would_be_ids")].toArray().size(), 2)
        << "INV-3: a batch preview reports its ids under `would_be_ids`\n"
        << QJsonDocument(env).toJson().toStdString();
    EXPECT_FALSE(env.contains(QStringLiteral("ids")))
        << "INV-3: the real write's key must not appear on a preview\n"
        << QJsonDocument(env).toJson().toStdString();
}

// ------------------------------------------------------------------ INV-4 --
TEST(RoadmapLogStorePreviewAndCounter, BatchRealWriteStillReportsIds) {
    Fixture fx;
    ASSERT_TRUE(fx.setUp(7));
    XdgRedirect redirect(fx.xdg.path());
    ASSERT_TRUE(migrateDefaultStore(fx.root));

    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdRoadmapLogAppendBatchForTest(
        batchReq(fx.root, {QStringLiteral("First real."),
                           QStringLiteral("Second real.")})).object();
    ASSERT_TRUE(env[QStringLiteral("ok")].toBool())
        << QJsonDocument(env).toJson().toStdString();
    assertStoreBranch(env, "append_batch");

    EXPECT_EQ(env[QStringLiteral("ids")].toArray().size(), 2)
        << "INV-4: a real batch allocates under `ids`";
    EXPECT_FALSE(env.contains(QStringLiteral("would_be_ids")))
        << "INV-4: the rename is preview-only";
}

// ------------------------------------------------------------------ INV-5 --
TEST(RoadmapLogStorePreviewAndCounter, BatchReconcilesTheCounterCache) {
    Fixture fx;
    // Seeded BELOW the fixture's own DEMO-0007 so the batch allocates 8 and 9
    // and the cache has somewhere to move to. This is the Games_Hub shape: the
    // counter trails the real high-water mark.
    ASSERT_TRUE(fx.setUp(7));
    XdgRedirect redirect(fx.xdg.path());
    ASSERT_TRUE(migrateDefaultStore(fx.root));

    RemoteControl rc(nullptr);
    const QJsonObject env = rc.cmdRoadmapLogAppendBatchForTest(
        batchReq(fx.root, {QStringLiteral("First batched."),
                           QStringLiteral("Second batched.")})).object();
    ASSERT_TRUE(env[QStringLiteral("ok")].toBool())
        << QJsonDocument(env).toJson().toStdString();
    assertStoreBranch(env, "append_batch");

    const QJsonArray ids = env[QStringLiteral("ids")].toArray();
    ASSERT_EQ(ids.size(), 2);

    // The highest id the batch allocated, taken from the envelope rather than
    // assumed, so the assertion does not silently depend on the seed.
    const QString lastId = ids[1].toString();
    const qint64 lastNum = lastId.section(QChar('-'), -1).toLongLong();
    ASSERT_GT(lastNum, 7);

    EXPECT_EQ(readCounter(fx.root), lastNum)
        << "INV-5: append_batch must reconcile .roadmap-counter to its highest "
           "allocation — op:append has done so since ANTS-4141 part 2, and a "
           "batch that does not leaves the cache stale until some later single "
           "append's scan repairs it";
    EXPECT_EQ(env[QStringLiteral("counter_advanced_to")].toInteger(), lastNum)
        << "INV-5: and it must SAY it moved — the batch envelope carried no "
           "counter fields at all, so a caller could not observe the "
           "difference without a follow-up call";
    EXPECT_EQ(env[QStringLiteral("counter_advanced_past")].toInteger(), 7);
}
