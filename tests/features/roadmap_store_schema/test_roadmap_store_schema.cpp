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
        auto s = store.addSection(projectId, QStringLiteral(""), QStringLiteral(""), 0, 0,
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

// INV-21 leg (a) — putItem() REACHES lanes/evidence/extras, and stores them
// canonical. Read back through raw SQL, not through a getter: the defect this
// locks is that the columns held their DDL defaults while every API call
// reported success, so a reader that round-trips the writer's own idea of the
// value cannot see it.
TEST(RoadmapStoreSchema, Inv21JsonColumnsAreWritable) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    QJsonObject extras;
    extras.insert(QStringLiteral("source_status"), QStringLiteral("🚧"));
    // 0.000001 is the ECMAScript fixed-versus-exponential boundary: JCS writes
    // it as 0.000001, QJsonDocument::toJson(Compact) as 1e-06. It is here so
    // that "canonicalised" is asserted rather than merely "written" — a writer
    // using Qt's compact form passes every other assertion in this test.
    extras.insert(QStringLiteral("tiny"), 0.000001);

    RoadmapStore::ItemWrite w;
    w.projectId = p;
    w.sectionId = s;
    w.position = 0;
    w.id = QStringLiteral("A-1");
    w.status = QStringLiteral("planned");
    w.headline = QStringLiteral("h");
    w.kind = QStringLiteral("implement");
    w.source = QStringLiteral("test");
    w.lanes = {QStringLiteral("vt"), QStringLiteral("chrome")};
    w.evidence = {QStringLiteral("docs/a.png")};
    w.extras = extras;
    const auto pk = f.store.putItem(w, &err);
    ASSERT_TRUE(pk.has_value()) << err.toStdString();

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral("SELECT lanes, evidence, extras FROM item WHERE item_pk = ?"));
    q.addBindValue(*pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());

    // JCS does not reorder an array — source order is the stored order.
    EXPECT_EQ(q.value(0).toString().toStdString(), std::string(R"(["vt","chrome"])"));
    EXPECT_EQ(q.value(1).toString().toStdString(), std::string(R"(["docs/a.png"])"));
    EXPECT_EQ(q.value(2).toString().toStdString(),
              std::string(R"({"source_status":"🚧","tiny":0.000001})"))
        << "extras must be stored in RFC 8785 form (ANTS-3756 section 2.3), which "
           "is what lets the export copy these bytes rather than transform them";
}

// INV-21 leg (b) — setItemField() reaches the same three columns, canonicalises
// what it is given, and refuses a value of the wrong SHAPE. Provenance stays
// per field (INV-10) across a JSON-column write, which is the half a writer
// that special-cases these columns is most likely to drop.
TEST(RoadmapStoreSchema, Inv21JsonColumnsAreEditable) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    QJsonObject prov;
    prov.insert(QStringLiteral("kind"), QStringLiteral("defaulted"));
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0, prov);

    ASSERT_TRUE(f.store.setItemField(pk, QStringLiteral("lanes"),
                                     QStringLiteral(R"(["core"])"), &err))
        << err.toStdString();
    ASSERT_TRUE(f.store.setItemField(pk, QStringLiteral("evidence"),
                                     QStringLiteral(R"(["b.log"])"), &err))
        << err.toStdString();
    // Deliberately NON-canonical input: keys out of order. The store holds
    // canonical bytes whatever the caller passed.
    ASSERT_TRUE(f.store.setItemField(pk, QStringLiteral("extras"),
                                     QStringLiteral(R"({"b":1,"a":2})"), &err))
        << err.toStdString();

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral(
        "SELECT lanes, evidence, extras, provenance FROM item WHERE item_pk = ?"));
    q.addBindValue(pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toString().toStdString(), std::string(R"(["core"])"));
    EXPECT_EQ(q.value(1).toString().toStdString(), std::string(R"(["b.log"])"));
    EXPECT_EQ(q.value(2).toString().toStdString(), std::string(R"({"a":2,"b":1})"))
        << "a JSON column is canonicalised on the way in, not stored verbatim";

    const QJsonObject got =
        QJsonDocument::fromJson(q.value(3).toString().toUtf8()).object();
    EXPECT_EQ(got.value(QStringLiteral("lanes")).toString().toStdString(),
              std::string("asserted"));
    EXPECT_EQ(got.value(QStringLiteral("kind")).toString().toStdString(),
              std::string("defaulted"))
        << "INV-10 still holds across a JSON-column write";

    // Shape, not just parseability: each of these parses as JSON and is still
    // wrong for the column it targets.
    EXPECT_FALSE(f.store.setItemField(pk, QStringLiteral("extras"),
                                      QStringLiteral("not json"), &err));
    EXPECT_FALSE(f.store.setItemField(pk, QStringLiteral("extras"),
                                      QStringLiteral("[1]"), &err))
        << "extras is an object";
    EXPECT_FALSE(f.store.setItemField(pk, QStringLiteral("lanes"),
                                      QStringLiteral(R"({"a":1})"), &err))
        << "lanes is an array";
    EXPECT_FALSE(f.store.setItemField(pk, QStringLiteral("lanes"),
                                      QStringLiteral("[1]"), &err))
        << "lanes is an array of STRINGS";

    // A refusal must not have written anything.
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toString().toStdString(), std::string(R"(["core"])"));
    EXPECT_EQ(q.value(2).toString().toStdString(), std::string(R"({"a":2,"b":1})"));
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

// --- ANTS-3765's four STORE-surface invariants -------------------------------
// Filed here with the rest of ANTS-3756's write-path invariants rather than in
// the migration directory, because they constrain store methods.
//
// They are ANTS-3765 INV-7/8/9/10 in that spec and INV-22/23/24/25 in
// ANTS-3756's own list, which is the numbering used here: this directory
// already tests ANTS-3756's INV-7, INV-8 and INV-10, all three of which mean
// something else entirely, so carrying the migration spec's numbers into these
// test names would leave two permanent meanings for one label.

// INV-22 (ANTS-3765 INV-7) — begin() refuses to nest, and reports.
TEST(RoadmapStoreSchema, Inv22BeginRefusesToNest) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();
    EXPECT_TRUE(f.store.inTransaction());

    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0);

    err.clear();
    EXPECT_FALSE(f.store.begin(&err))
        << "a nested begin() must refuse — a no-op here would make the caller's "
           "commit() end a transaction it did not open";
    EXPECT_FALSE(err.isEmpty()) << "the refusal must REPORT";
    EXPECT_TRUE(f.store.inTransaction())
        << "a refused begin() must leave the open transaction alone";

    // The first transaction is still the live one, and still commits its writes.
    err.clear();
    EXPECT_TRUE(f.store.commit(&err)) << err.toStdString();
    EXPECT_FALSE(f.store.inTransaction());

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral("SELECT COUNT(*) FROM item WHERE item_pk = ?"));
    q.addBindValue(pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1)
        << "the first transaction's writes must survive its own commit";

    // commit()/rollback() with none open refuse rather than silently succeeding.
    err.clear();
    EXPECT_FALSE(f.store.commit(&err)) << "commit() with no transaction open";
    EXPECT_FALSE(err.isEmpty());
    err.clear();
    EXPECT_FALSE(f.store.rollback(&err)) << "rollback() with no transaction open";
    EXPECT_FALSE(err.isEmpty());
}

// INV-23 (ANTS-3765 INV-8) — putItem() is atomic with or without an enclosing
// transaction, and never rolls back one it does not own.
TEST(RoadmapStoreSchema, Inv23PutItemNeverRollsBackATransactionItDoesNotOwn) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    // (a) the standalone case: no transaction open, so putItem owns and commits
    // its own, and leaves none open behind it.
    const qint64 standalone = f.item(p, s, QStringLiteral("A-1"), 0);
    EXPECT_GT(standalone, 0);
    EXPECT_FALSE(f.store.inTransaction());

    // (b) inside a transaction the caller rolls back: neither the item nor its
    // element row survives.
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();
    const qint64 inner = f.item(p, s, QStringLiteral("A-2"), 1);
    EXPECT_GT(inner, 0);
    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();

    QSqlQuery gone(f.store.db());
    ASSERT_TRUE(gone.exec(QStringLiteral("SELECT COUNT(*) FROM item WHERE id = 'A-2'")));
    ASSERT_TRUE(gone.next());
    EXPECT_EQ(gone.value(0).toInt(), 0) << "putItem() must not commit inside a "
                                           "caller's transaction";
    QSqlQuery goneElem(f.store.db());
    ASSERT_TRUE(goneElem.exec(QStringLiteral(
        "SELECT COUNT(*) FROM element WHERE position = 1 AND kind = 'item'")));
    ASSERT_TRUE(goneElem.next());
    EXPECT_EQ(goneElem.value(0).toInt(), 0) << "nor its element row";

    // (c) the leg that matters — a FAILING putItem inside an open transaction
    // must not abort the caller's transaction from the inside. Without this,
    // load() carries on believing it is in a transaction, every later write
    // autocommits and PERSISTS, and the report says the load was clean.
    ASSERT_TRUE(f.store.begin(&err)) << err.toStdString();
    RoadmapStore::ItemWrite bad;
    bad.projectId = p;
    bad.sectionId = s;
    bad.position = 0;             // collides with A-1: UNIQUE (section_id, position)
    bad.id = QStringLiteral("A-3");
    bad.status = QStringLiteral("planned");
    bad.headline = QStringLiteral("h");
    bad.kind = QStringLiteral("implement");
    bad.source = QStringLiteral("test");
    err.clear();
    EXPECT_FALSE(f.store.putItem(bad, &err).has_value());
    EXPECT_FALSE(err.isEmpty());
    EXPECT_TRUE(f.store.inTransaction())
        << "a failed putItem() must leave the caller's transaction OPEN — an "
           "internal ROLLBACK here silently drops every later write into "
           "autocommit, where it persists";

    // The caller's own unwind is what must be observed, and it must still work.
    const qint64 after = f.item(p, s, QStringLiteral("A-4"), 2);
    EXPECT_GT(after, 0);
    ASSERT_TRUE(f.store.rollback(&err)) << err.toStdString();

    QSqlQuery survivors(f.store.db());
    ASSERT_TRUE(survivors.exec(QStringLiteral("SELECT COUNT(*) FROM item")));
    ASSERT_TRUE(survivors.next());
    EXPECT_EQ(survivors.value(0).toInt(), 1)
        << "only the standalone item may survive: the write after the failure "
           "must have been rolled back by the CALLER's rollback()";
}

// INV-24 (ANTS-3765 INV-9) — the two JSON-writing methods store canonical JSON;
// the two prose-writing ones store their text verbatim.
TEST(RoadmapStoreSchema, Inv24JsonWritersCanonicaliseAndProseWritersDoNot) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    // setLegend() — canonical: keys sorted, no whitespace.
    QJsonObject legend;
    legend.insert(QStringLiteral("b"), QStringLiteral("in progress"));
    legend.insert(QStringLiteral("a"), QStringLiteral("planned"));
    ASSERT_TRUE(f.store.setLegend(p, legend, &err)) << err.toStdString();

    QSqlQuery lq(f.store.db());
    lq.prepare(QStringLiteral("SELECT legend FROM project WHERE project_id = ?"));
    lq.addBindValue(p);
    ASSERT_TRUE(lq.exec());
    ASSERT_TRUE(lq.next());
    EXPECT_EQ(lq.value(0).toString().toStdString(),
              std::string(R"({"a":"planned","b":"in progress"})"));

    // addElement(kind='table') — canonical, from deliberately out-of-order input.
    ASSERT_TRUE(f.store.addElement(s, 0, QStringLiteral("table"),
                                   QStringLiteral(R"({ "rows": [["x"]], "header": ["h"] })"),
                                   &err))
        << err.toStdString();

    // addElement(kind='narration') — VERBATIM, even when the prose happens to
    // look like JSON. ANTS-3756 § 2.3 calls canonicalising prose undefined
    // rather than merely wasteful.
    const QString prose = QStringLiteral(R"({"b":1,"a":2}  trailing prose)");
    ASSERT_TRUE(f.store.addElement(s, 1, QStringLiteral("narration"), prose, &err))
        << err.toStdString();

    QSqlQuery eq(f.store.db());
    eq.prepare(QStringLiteral(
        "SELECT kind, payload FROM element WHERE section_id = ? ORDER BY position"));
    eq.addBindValue(s);
    ASSERT_TRUE(eq.exec());
    ASSERT_TRUE(eq.next());
    EXPECT_EQ(eq.value(0).toString().toStdString(), std::string("table"));
    EXPECT_EQ(eq.value(1).toString().toStdString(),
              std::string(R"({"header":["h"],"rows":[["x"]]})"));
    ASSERT_TRUE(eq.next());
    EXPECT_EQ(eq.value(0).toString().toStdString(), std::string("narration"));
    EXPECT_EQ(eq.value(1).toString().toStdString(), prose.toStdString())
        << "narration payload must round-trip byte for byte";

    // setSectionIntro() — prose, verbatim, likewise.
    const QString intro = QStringLiteral("  Two  spaces and {\"b\":1,\"a\":2}\n");
    ASSERT_TRUE(f.store.setSectionIntro(s, intro, &err)) << err.toStdString();
    QSqlQuery iq(f.store.db());
    iq.prepare(QStringLiteral("SELECT intro FROM section WHERE section_id = ?"));
    iq.addBindValue(s);
    ASSERT_TRUE(iq.exec());
    ASSERT_TRUE(iq.next());
    EXPECT_EQ(iq.value(0).toString().toStdString(), intro.toStdString());
}

// INV-25 (ANTS-3765 INV-10) — addElement() cannot file an item and fileItem()
// cannot double-file one, so putItem()/fileItem() stay the only filing paths.
TEST(RoadmapStoreSchema, Inv25FilingPathsAreClosed) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);
    const qint64 pk = f.item(p, s, QStringLiteral("A-1"), 0);

    // addElement() refuses kind='item' OUTRIGHT — it has no item_pk parameter,
    // so it could not produce a well-formed filing even if it tried.
    //
    // The assertion is on the refusal's SHAPE, not merely on its existence, and
    // that is forced by the DDL: `CHECK ((kind='item') = (item_pk IS NOT NULL))`
    // refuses this insert too, so a method that passed `kind` straight through
    // would also return false. What distinguishes them is which layer spoke —
    // a misuse of the API, or a constraint violation the caller now has to
    // parse — so the error text is where the invariant is observable.
    err.clear();
    EXPECT_FALSE(f.store.addElement(s, 1, QStringLiteral("item"),
                                    QStringLiteral("payload"), &err));
    EXPECT_TRUE(err.contains(QStringLiteral("cannot file an item")))
        << "must be refused by addElement() as a misuse, not by the DDL CHECK "
           "as a constraint violation; got: " << err.toStdString();

    // fileItem() refuses an item that is already filed — same reasoning:
    // elem_item_uq also catches it, as a constraint violation rather than a
    // reported refusal.
    err.clear();
    EXPECT_FALSE(f.store.fileItem(pk, s, 1, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("already filed")))
        << "must be refused by fileItem()'s own check; got: " << err.toStdString();

    // unfileItem() is the inverse and the only way back: it removes the filing
    // of ONE item without touching the section's other element rows, which is
    // what the rebuild needs for an item whose stored section the plan no
    // longer carries.
    ASSERT_TRUE(f.store.addElement(s, 9, QStringLiteral("narration"),
                                   QStringLiteral("keep me"), &err))
        << err.toStdString();
    ASSERT_TRUE(f.store.unfileItem(pk, &err)) << err.toStdString();

    QSqlQuery kept(f.store.db());
    kept.prepare(QStringLiteral("SELECT COUNT(*) FROM element WHERE section_id = ?"));
    kept.addBindValue(s);
    ASSERT_TRUE(kept.exec());
    ASSERT_TRUE(kept.next());
    EXPECT_EQ(kept.value(0).toInt(), 1)
        << "unfileItem() must take the item's filing and nothing else — the "
           "narration row in the same section is payload nothing re-inserts";

    // And fileItem() FILES an item that is not filed — the case § 2.6's element
    // rebuild needs and the whole reason the method exists.
    err.clear();
    ASSERT_TRUE(f.store.fileItem(pk, s, 3, &err)) << err.toStdString();

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral(
        "SELECT COUNT(*), MIN(position) FROM element WHERE kind = 'item' AND item_pk = ?"));
    q.addBindValue(pk);
    ASSERT_TRUE(q.exec());
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1);
    EXPECT_EQ(q.value(1).toInt(), 3);
}

// ============================================================================
// ANTS-3782 — section.source_path: the column, its writer and its reader.
// Numbered from 26, past ANTS-3756's highest, so no bare Inv<N> in this file
// resolves two ways (that spec already has an INV-15).
// ============================================================================

// -------------------------------------------------------- ANTS-3782 INV-26 --
// The column is reachable through the TYPED surface, for both the NULL and the
// path case. Asserted against raw SQL, never a round-trip through the writer: a
// writer compared with its own idea of the value cannot distinguish a stored
// value from a default, which is the ANTS-3767 failure one column along.
TEST(RoadmapStoreSchema, Inv26SourcePathReadableThroughSectionRow) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 pid = f.project(QStringLiteral("p"));

    const auto live = f.store.addSection(pid, QStringLiteral("live"),
                                         QStringLiteral("Live"), 2, 0,
                                         std::nullopt, &err);
    ASSERT_TRUE(live.has_value()) << err.toStdString();
    const auto arch = f.store.addSection(pid, QStringLiteral("0-6-features"),
                                         QStringLiteral("Features"), 3, 1,
                                         std::nullopt, &err);
    ASSERT_TRUE(arch.has_value()) << err.toStdString();

    // A section with no source written at all keeps the DDL NULL...
    auto row = f.store.readSection(*live, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    EXPECT_FALSE(row->sourcePath.has_value())
        << "INV-26: an unwritten column reads back as nullopt, which is exactly "
           "what the live roadmap means — so no backfill is ever needed";

    // ...and one written explicitly reads back byte for byte.
    const QString rel = QStringLiteral("docs/roadmap/0.6.md");
    ASSERT_TRUE(f.store.setSectionSource(*arch, rel, &err)) << err.toStdString();
    row = f.store.readSection(*arch, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    ASSERT_TRUE(row->sourcePath.has_value())
        << "INV-26: breaks when SectionRow gains the field but readSection()'s "
           "SELECT does not — every row then reads back nullopt, and the "
           "live-roadmap leg PASSES while this one fails";
    EXPECT_EQ(*row->sourcePath, rel);

    // The oracle is raw SQL, not the writer.
    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT source_path FROM section WHERE slug = '0-6-features'")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toString(), rel);
    ASSERT_TRUE(q.exec(QStringLiteral(
        "SELECT source_path IS NULL FROM section WHERE slug = 'live'")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), 1) << "SQL NULL, not the empty string";

    // Writing nullopt back returns the row to the live-roadmap state.
    ASSERT_TRUE(f.store.setSectionSource(*arch, std::nullopt, &err))
        << err.toStdString();
    row = f.store.readSection(*arch, &err);
    ASSERT_TRUE(row.has_value());
    EXPECT_FALSE(row->sourcePath.has_value())
        << "a rotated section that becomes live again must clear to NULL";
}

// An ENGAGED optional holding an empty string stores '', and does NOT fold to
// NULL the way setSectionIntro() deliberately does. Folding would make
// "unplaceable, stored anyway" indistinguishable from "the live roadmap".
TEST(RoadmapStoreSchema, Inv26EmptySourcePathIsNotFoldedToNull) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    const qint64 pid = f.project(QStringLiteral("p"));
    const auto sid = f.store.addSection(pid, QStringLiteral("s"),
                                        QStringLiteral("S"), 2, 0, std::nullopt, &err);
    ASSERT_TRUE(sid.has_value()) << err.toStdString();

    ASSERT_TRUE(f.store.setSectionSource(*sid, QString(), &err)) << err.toStdString();
    const auto row = f.store.readSection(*sid, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    ASSERT_TRUE(row->sourcePath.has_value())
        << "INV-26: breaks when this method inherits setSectionIntro()'s "
           "empty-to-NULL fold — '' is a meaningless INTRO and a WRONG source "
           "path, and collapsing them loses the one distinction std::optional "
           "is here to carry";
    EXPECT_TRUE(row->sourcePath->isEmpty());
}

// ------------------------------- ANTS-3782 INV-27 / ANTS-3796 INV-6 --------
// This change does not move the schema version. ANTS-3796 INV-6 is the same
// assertion for section.position and rides on this test rather than
// duplicating it — the argument is identical (no store is reachable from
// user-facing code, so a bump would manufacture an upgrade case nothing
// implements in order to migrate zero stores), and a second copy would be a
// second thing to keep true.
//
// ANTS-3796's INV-6 deliberately drops the leg ANTS-3782's carried, that the
// three export goldens still import: that cannot hold across ANTS-3796,
// because the record shape is what changed and § 4 regenerates them.
//
// **The `kSchemaVersion == 1` leg is RETIRED (ANTS-3815).** That bump is the one
// this test's old message named as entitled to move the constant, and it was made
// deliberately, together with the rung that climbs to it. The name stays: it is a
// handle cited from two specs' invariants (ANTS-3782 INV-27, ANTS-3796 INV-6) and
// renaming it strands both, so it becomes historical exactly as
// `Inv8DdlBuiltAndClimbedStoresMatch` already is. What survives is the leg that
// was always the load-bearing one — a store this build CREATES is stamped with
// this build's version, whatever that version is.
TEST(RoadmapStoreSchema, Inv27SchemaVersionStillOne) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();
    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral("PRAGMA user_version")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toInt(), RoadmapStore::kSchemaVersion)
        << "a store this build creates must be stamped with this build's version";
}

// ------------------------------------------- ANTS-3815 INV-1 ---------------
// project.source_format exists as TEXT NOT NULL DEFAULT '' with a CHECK admitting
// exactly '' and detectRoadmapFormat()'s three dialects.
//
// This test builds through the DDL and NEVER climbs, which is why a defective
// RUNG leaves it green — that mutation is ANTS-3815 INV-4's to catch, in
// roadmap_store_upgrade.
TEST(RoadmapStoreSchema, Ants3815Inv1SourceFormatColumn) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.store.open(&err)) << err.toStdString();

    // (a) declared shape. `dflt_value` holds the default EXPRESSION, not an empty
    // string: for DEFAULT '' it is the two-character SQL text '' — measured,
    // quote(dflt_value) returns ''''''. Asserting an empty string here would be
    // unsatisfiable against a correct column.
    bool found = false;
    {
        QSqlQuery q(f.store.db());
        ASSERT_TRUE(q.exec(QStringLiteral("PRAGMA table_info(project)")));
        while (q.next()) {
            if (q.value(1).toString() != QStringLiteral("source_format"))
                continue;
            found = true;
            EXPECT_EQ(q.value(2).toString().toStdString(), std::string("TEXT"));
            EXPECT_EQ(q.value(3).toInt(), 1) << "the column must be NOT NULL";
            EXPECT_EQ(q.value(4).toString().toStdString(), std::string("''"))
                << "dflt_value is the default expression, so DEFAULT '' reads as "
                   "the two-character text ''";
        }
    }
    ASSERT_TRUE(found) << "project has no source_format column";

    // (b) the CHECK. The row is INSERTED FIRST, and that is not incidental: an
    // UPDATE matching no row SUCCEEDS in SQLite — the very behaviour
    // setProjectSourceFormat() checks numRowsAffected() for — so against the empty
    // project table a fresh store starts with, this leg would be vacuously green.
    const qint64 pid = f.project(QStringLiteral("p"));
    ASSERT_GT(pid, 0);

    for (const QString &ok : {QStringLiteral(""), QStringLiteral("ants-v1"),
                              QStringLiteral("github-task-list"),
                              QStringLiteral("pass-headings")}) {
        EXPECT_TRUE(f.store.setProjectSourceFormat(pid, ok, &err))
            << ok.toStdString() << ": " << err.toStdString();
    }

    err.clear();
    EXPECT_FALSE(f.store.setProjectSourceFormat(pid, QStringLiteral("klingon"), &err))
        << "the CHECK must refuse a format outside detectRoadmapFormat()'s range";
    EXPECT_FALSE(err.isEmpty()) << "a refusal must carry its reason";

    // The refused write left the last accepted value in place.
    const auto row = f.store.readProject(pid, &err);
    ASSERT_TRUE(row.has_value()) << err.toStdString();
    EXPECT_EQ(row->sourceFormat.toStdString(), std::string("pass-headings"));
}
