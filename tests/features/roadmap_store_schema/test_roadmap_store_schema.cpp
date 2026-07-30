// Feature-conformance test for ANTS-3756 INV-6/7/8/10/11/14/17/20 — the
// roadmap store's schema, location and write path.
// Contract: tests/features/roadmap_store_schema/spec.md

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <sys/stat.h>
#include <unistd.h>

namespace {

struct Fixture {
    QTemporaryDir dir;
    RoadmapStore store;

    explicit Fixture(qint64 historyCap = RoadmapStore::kDefaultHistoryCapBytes)
        : store(dir.path() + QStringLiteral("/roadmap.sqlite"), historyCap) {}

    qint64 project(const QString &slug) {
        const QString root = dir.path() + QStringLiteral("/") + slug;
        QDir().mkpath(root);
        QString err;
        auto pk = store.registerProject(root, slug, slug, &err);
        EXPECT_TRUE(pk.has_value()) << err.toStdString();
        return pk.value_or(-1);
    }

    qint64 section(qint64 projectId) {
        QString err;
        auto s = store.addSection(projectId, QStringLiteral(""), QStringLiteral(""), 0,
                                  std::nullopt, &err);
        EXPECT_TRUE(s.has_value()) << err.toStdString();
        return s.value_or(-1);
    }

    qint64 item(qint64 projectId, qint64 sectionId, const QString &id, int position,
                const QJsonObject &provenance = {}) {
        RoadmapStore::ItemWrite w;
        w.projectId = projectId;
        w.sectionId = sectionId;
        w.position = position;
        w.id = id;
        w.status = QStringLiteral("planned");
        w.headline = QStringLiteral("h");
        w.kind = QStringLiteral("implement");
        w.source = QStringLiteral("test");
        w.provenance = provenance;
        QString err;
        auto pk = store.putItem(w, &err);
        EXPECT_TRUE(pk.has_value()) << err.toStdString();
        return pk.value_or(-1);
    }
};

// The real Unix mode, not Qt's permission enum. QFileDevice reports 0600 as
// ReadOwner|WriteOwner|ReadUser|WriteUser (0x6600) because the Owner and User
// flags are the same bits on Unix — easy to half-match and get a passing test
// that means something else. The invariant says "mode 0600"; stat(2) says it.
int fileMode(const QString &path) {
    struct stat st {};
    if (::stat(path.toUtf8().constData(), &st) != 0)
        return -1;
    return static_cast<int>(st.st_mode & 07777);
}

}  // namespace

// INV-7 — resolved under GenericDataLocation, and NEITHER cache root is a
// prefix. Asserted on the runtime path, not by grepping for a constant.
TEST(RoadmapStoreSchema, Inv7PathIsDataLocationNeverCache) {
    const QString p = RoadmapStore::defaultPath();
    const QString expected =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
        QStringLiteral("/ants-terminal/roadmap.sqlite");
    EXPECT_EQ(p.toStdString(), expected.toStdString());

    // Direction matters: ~/.cache/ants-terminal/roadmap.sqlite is NOT a prefix
    // of the cache root, so the reversed comparison passes for exactly the
    // placement this invariant forbids.
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString genericCache =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QStringLiteral("/ants-terminal");
    EXPECT_FALSE(p.startsWith(cache)) << "store must not live under " << cache.toStdString();
    EXPECT_FALSE(p.startsWith(genericCache))
        << "store must not live under " << genericCache.toStdString();
}

// INV-8 — canonical root keying, all three legs.
TEST(RoadmapStoreSchema, Inv8ProjectKeyedOnCanonicalRoot) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    const QString real = f.dir.path() + QStringLiteral("/real");
    ASSERT_TRUE(QDir().mkpath(real));
    const QString link = f.dir.path() + QStringLiteral("/link");
    ASSERT_EQ(::symlink(real.toUtf8().constData(), link.toUtf8().constData()), 0);

    // (a) the symlink and the real path are ONE project.
    auto viaReal = f.store.registerProject(real, QStringLiteral("R"), QStringLiteral("r"), &err);
    ASSERT_TRUE(viaReal.has_value()) << err.toStdString();
    auto viaLink = f.store.registerProject(link, QStringLiteral("R"), QStringLiteral("r"), &err);
    ASSERT_TRUE(viaLink.has_value()) << err.toStdString();
    EXPECT_EQ(*viaReal, *viaLink) << "a symlinked root must not shadow the real one";

    // (b) two genuinely distinct roots are TWO projects.
    const QString other = f.dir.path() + QStringLiteral("/other");
    ASSERT_TRUE(QDir().mkpath(other));
    auto second = f.store.registerProject(other, QStringLiteral("O"), QStringLiteral("o"), &err);
    ASSERT_TRUE(second.has_value()) << err.toStdString();
    EXPECT_NE(*viaReal, *second);

    // (c) a root that cannot be canonicalised is REFUSED, and writes no row.
    // canonicalFilePath() returns "" for a non-existent path; stored unchecked,
    // '' under UNIQUE fuses every missing root into one project.
    QSqlQuery before(f.store.db());
    ASSERT_TRUE(before.exec(QStringLiteral("SELECT COUNT(*) FROM project")));
    ASSERT_TRUE(before.next());
    const int rowsBefore = before.value(0).toInt();

    err.clear();
    EXPECT_FALSE(f.store
                     .registerProject(f.dir.path() + QStringLiteral("/ghost-a"),
                                      QStringLiteral("A"), QStringLiteral("ga"), &err)
                     .has_value());
    EXPECT_FALSE(f.store
                     .registerProject(f.dir.path() + QStringLiteral("/ghost-b"),
                                      QStringLiteral("B"), QStringLiteral("gb"), &err)
                     .has_value());

    QSqlQuery after(f.store.db());
    ASSERT_TRUE(after.exec(QStringLiteral("SELECT COUNT(*) FROM project")));
    ASSERT_TRUE(after.next());
    EXPECT_EQ(after.value(0).toInt(), rowsBefore)
        << "a refused root must not write a row";
}

// INV-11 — every closed enum in its OWN column is refused at the storage layer.
TEST(RoadmapStoreSchema, Inv11ClosedEnumsRejectedByTheEngine) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    const qint64 a = f.item(p, s, QStringLiteral("A-1"), 0);
    const qint64 b = f.item(p, s, QStringLiteral("A-2"), 1);

    const auto refuses = [&](const QString &sql) {
        QSqlQuery q(f.store.db());
        q.prepare(sql);
        q.addBindValue(p);
        return !q.exec();
    };
    const QString base = QStringLiteral(
        "INSERT INTO item (project_id, id, id_origin, status, headline, kind, "
        "source%1) VALUES (?, 'E-1', %2)");

    EXPECT_TRUE(refuses(base.arg(QString(), QStringLiteral(
        "'parsed', 'nonsense', 'h', 'implement', 't'")))) << "status enum";
    EXPECT_TRUE(refuses(base.arg(QString(), QStringLiteral(
        "'parsed', 'planned', 'h', 'nonsense', 't'")))) << "kind enum";
    EXPECT_TRUE(refuses(base.arg(QString(), QStringLiteral(
        "'nonsense', 'planned', 'h', 'implement', 't'")))) << "id_origin enum";
    EXPECT_TRUE(refuses(base.arg(QStringLiteral(", visibility"), QStringLiteral(
        "'parsed', 'planned', 'h', 'implement', 't', 'nonsense'")))) << "visibility enum";
    EXPECT_TRUE(refuses(base.arg(QStringLiteral(", priority"), QStringLiteral(
        "'parsed', 'planned', 'h', 'implement', 't', 9")))) << "priority range";

    QSqlQuery e(f.store.db());
    e.prepare(QStringLiteral(
        "INSERT INTO element (section_id, position, kind, payload) VALUES (?, 99, 'nonsense', 'x')"));
    e.addBindValue(s);
    EXPECT_FALSE(e.exec()) << "element.kind enum";

    QSqlQuery r(f.store.db());
    r.prepare(QStringLiteral(
        "INSERT INTO relationship (type, src_pk, dst_pk) VALUES ('nonsense', ?, ?)"));
    r.addBindValue(a);
    r.addBindValue(b);
    EXPECT_FALSE(r.exec()) << "relationship.type enum";
}

// INV-6 — relates-to stored once, normalised on STABLE identity.
TEST(RoadmapStoreSchema, Inv6RelatesToNormalisedOnStableIdentity) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    // zzz sorts AFTER aaa on (export_slug, id_fold). Write the HIGHER one
    // first: writing the lower first would pass against a writer that merely
    // rejects the second edge without normalising anything.
    const qint64 high = f.item(p, s, QStringLiteral("zzz-1"), 0);
    const qint64 low = f.item(p, s, QStringLiteral("aaa-1"), 1);

    ASSERT_TRUE(f.store.relateItems(QStringLiteral("relates-to"), high, low, &err))
        << err.toStdString();
    // The reverse edge is the same logical relationship — still one row.
    ASSERT_TRUE(f.store.relateItems(QStringLiteral("relates-to"), low, high, &err))
        << err.toStdString();

    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT COUNT(*), MIN(src_pk) FROM relationship WHERE type = 'relates-to'")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1) << "a symmetric edge is stored once";
    EXPECT_EQ(q.value(1).toLongLong(), low)
        << "src must be the LOWER-sorting endpoint by (export_slug, id_fold)";
}

// INV-6, second leg — an unresolved cross-project endpoint keeps the LOCAL
// item as src_pk whichever way the pair would sort. src_pk is NOT NULL, so the
// unconditional rule is unsatisfiable here.
TEST(RoadmapStoreSchema, Inv6CrossProjectKeepsLocalAsSrc) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("zzz"));
    const qint64 s = f.section(p);
    const qint64 localItem = f.item(p, s, QStringLiteral("zzz-1"), 0);

    // "aaa" sorts before "zzz", so an unconditional rule would want the far
    // side as src — which cannot be expressed.
    ASSERT_TRUE(f.store.relateCrossProject(QStringLiteral("relates-to"), localItem,
                                           QStringLiteral("aaa"),
                                           QStringLiteral("AAA-1"), &err))
        << err.toStdString();

    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT src_pk, dst_project, dst_id_fold, dst_pk FROM relationship")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toLongLong(), localItem);
    EXPECT_EQ(q.value(1).toString().toStdString(), std::string("aaa"));
    EXPECT_EQ(q.value(2).toString().toStdString(), std::string("aaa-1"))
        << "the far id is stored FOLDED";
    EXPECT_TRUE(q.value(3).isNull()) << "an unresolved edge has no dst_pk";
}

// INV-10 — provenance is per FIELD, in BOTH directions.
TEST(RoadmapStoreSchema, Inv10ProvenanceIsPerField) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    QJsonObject prov;
    prov.insert(QStringLiteral("kind"), QStringLiteral("defaulted"));
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0, prov);

    ASSERT_TRUE(f.store.setItemField(pk, QStringLiteral("headline"),
                                     QStringLiteral("edited"), &err))
        << err.toStdString();

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral("SELECT headline, provenance FROM item WHERE item_pk = ?"));
    q.addBindValue(pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toString().toStdString(), std::string("edited"));

    const QJsonObject got =
        QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object();
    EXPECT_EQ(got.value(QStringLiteral("headline")).toString().toStdString(),
              std::string("asserted"))
        << "the edited field must become asserted";
    EXPECT_EQ(got.value(QStringLiteral("kind")).toString().toStdString(),
              std::string("defaulted"))
        << "every OTHER field's provenance must be left untouched — a one-sided "
           "assertion certifies a writer that never updates provenance at all";
}

// INV-14 leg (a) — below the bound NOTHING is evicted.
TEST(RoadmapStoreSchema, Inv14LosslessBelowTheCap) {
    Fixture f(1024 * 1024);  // generous: 60 small revisions sit well below it
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0);

    for (int i = 0; i < 60; ++i) {
        const QString at = QStringLiteral("2026-07-30T09:15:00Z");
        ASSERT_TRUE(f.store.appendHistory(pk, at, i, QStringLiteral("status"),
                                          QStringLiteral("planned"),
                                          QStringLiteral("shipped"), &err))
            << "revision " << i << ": " << err.toStdString();
    }

    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral("SELECT COUNT(*) FROM history")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 60)
        << "a per-item cap evicting oldest-first destroys the only copy of what "
           "the model's section 6 says the export exists to preserve";
}

// INV-14 leg (b) — AT the bound the history write fails and REPORTS, while the
// item write it accompanies still succeeds.
TEST(RoadmapStoreSchema, Inv14AtCapRefusesAndReports) {
    Fixture f(64);  // tiny: a couple of revisions crosses it
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0);

    int accepted = 0;
    bool refused = false;
    for (int i = 0; i < 20 && !refused; ++i) {
        err.clear();
        if (f.store.appendHistory(pk, QStringLiteral("2026-07-30T09:15:00Z"), i,
                                  QStringLiteral("status"), QStringLiteral("planned"),
                                  QStringLiteral("shipped"), &err))
            ++accepted;
        else
            refused = true;
    }
    EXPECT_TRUE(refused) << "the cap must eventually refuse";
    EXPECT_GT(accepted, 0) << "writes below the cap must succeed";
    EXPECT_FALSE(err.isEmpty())
        << "the refusal must REPORT — a silently dropped revision is "
           "indistinguishable from one that never happened";

    // The item write it accompanies still succeeds.
    err.clear();
    EXPECT_TRUE(f.store.setItemField(pk, QStringLiteral("status"),
                                     QStringLiteral("shipped"), &err))
        << err.toStdString();
}

// INV-17 — the store and BOTH WAL sidecars are 0600, asserted with the
// connection still open after a write (it is the write that creates them).
TEST(RoadmapStoreSchema, Inv17StoreAndSidecarsAreOwnerOnly) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    f.section(p);  // a committed write, so -wal / -shm exist

    const QString base = f.store.path();
    const int want = 0600;
    for (const QString &suffix :
         {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        const QString path = base + suffix;
        ASSERT_TRUE(QFile::exists(path))
            << path.toStdString()
            << " must exist — a write through an OPEN WAL connection creates the "
               "sidecars; a checkpoint does not, and closing deletes them";
        EXPECT_EQ(fileMode(path), want)
            << path.toStdString()
            << " must be 0600 — the sidecars carry the same content, including "
               "visibility:internal items";
    }
}

// INV-20 — every item is filed exactly once.
TEST(RoadmapStoreSchema, Inv20ItemFiledExactlyOnce) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0);

    // (a) exactly one element, in the section written to.
    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral(
        "SELECT COUNT(*), MIN(section_id) FROM element WHERE kind = 'item' AND item_pk = ?"));
    q.addBindValue(pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1);
    EXPECT_EQ(q.value(1).toLongLong(), s);

    // (b) a SECOND filing is refused by elem_item_uq. A section_id column on
    // item could not express this at all.
    QSqlQuery dup(f.store.db());
    dup.prepare(QStringLiteral(
        "INSERT INTO element (section_id, position, kind, item_pk) VALUES (?, 77, 'item', ?)"));
    dup.addBindValue(s);
    dup.addBindValue(pk);
    EXPECT_FALSE(dup.exec()) << "an item must not be filed twice";

    // (c) a failed element insert rolls the ITEM back — no unfiled item
    // survives. Position 0 is taken, so UNIQUE(section_id, position) fails the
    // element while the item insert has already succeeded in the same tx.
    RoadmapStore::ItemWrite w;
    w.projectId = p;
    w.sectionId = s;
    w.position = 0;  // collides
    w.id = QStringLiteral("A-2");
    w.status = QStringLiteral("planned");
    w.headline = QStringLiteral("h");
    w.kind = QStringLiteral("implement");
    w.source = QStringLiteral("test");
    err.clear();
    EXPECT_FALSE(f.store.putItem(w, &err).has_value());

    QSqlQuery orphan(f.store.db());
    ASSERT_TRUE(orphan.exec(QStringLiteral("SELECT COUNT(*) FROM item WHERE id = 'A-2'")));
    ASSERT_TRUE(orphan.next());
    EXPECT_EQ(orphan.value(0).toInt(), 0)
        << "a failed element insert must roll the item back, or an unfiled item "
           "survives — which is what the removed NOT NULL column used to prevent";
}
