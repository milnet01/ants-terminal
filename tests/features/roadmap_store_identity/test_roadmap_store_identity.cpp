// Feature-conformance test for ANTS-3756 INV-3 / INV-4 — item identity in the
// roadmap store. Contract: tests/features/roadmap_store_identity/spec.md
//
// Behavioural, not source-scrape: the invariants are about what SQLite accepts
// and refuses, so the only honest test opens a real store.

#include <gtest/gtest.h>

#include "roadmapstore.h"

#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

namespace {

struct Fixture {
    QTemporaryDir dir;
    RoadmapStore store;
    Fixture() : store(dir.path() + QStringLiteral("/roadmap.sqlite")) {}

    bool open(QString *err) { return store.open(err); }

    qint64 project(const QString &slug) {
        // The root must exist: INV-8 refuses a path that does not canonicalise.
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

    RoadmapStore::ItemWrite item(qint64 projectId, qint64 sectionId,
                                 const QString &id, int position,
                                 const QString &origin = QStringLiteral("parsed")) {
        RoadmapStore::ItemWrite w;
        w.projectId = projectId;
        w.sectionId = sectionId;
        w.position = position;
        w.id = id;
        w.idOrigin = origin;
        w.status = QStringLiteral("planned");
        w.headline = QStringLiteral("h");
        w.kind = QStringLiteral("implement");
        w.source = QStringLiteral("test");
        return w;
    }
};

}  // namespace

// INV-3 leg 1 — case-folded identity WITHIN a project.
TEST(RoadmapStoreIdentity, Inv3FoldedIdCollidesInOneProject) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.open(&err)) << err.toStdString();

    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    ASSERT_TRUE(f.store.putItem(f.item(p, s, QStringLiteral("Sh-1"), 0), &err).has_value())
        << err.toStdString();

    err.clear();
    EXPECT_FALSE(f.store.putItem(f.item(p, s, QStringLiteral("SH-1"), 1), &err).has_value())
        << "SH-1 must collide with Sh-1 inside one project";
    EXPECT_TRUE(err.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive))
        << "expected a uniqueness violation, got: " << err.toStdString();
}

// INV-3 leg 2 — the SAME pair in two projects is two items, not a collision.
// roadmap-data-model.md 7.1: an id is unique within its project, not the store.
TEST(RoadmapStoreIdentity, Inv3SameIdInTwoProjectsIsAllowed) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.open(&err)) << err.toStdString();

    const qint64 a = f.project(QStringLiteral("alpha"));
    const qint64 b = f.project(QStringLiteral("beta"));
    const qint64 sa = f.section(a);
    const qint64 sb = f.section(b);

    EXPECT_TRUE(f.store.putItem(f.item(a, sa, QStringLiteral("Sh-1"), 0), &err).has_value())
        << err.toStdString();
    err.clear();
    EXPECT_TRUE(f.store.putItem(f.item(b, sb, QStringLiteral("Sh-1"), 0), &err).has_value())
        << "a store keyed on id_fold alone would reject a corpus the model requires: "
        << err.toStdString();
}

// INV-3 leg 3 — the folding is STRUCTURAL. A writer cannot store an unfolded
// key, because id_fold is GENERATED and SQLite refuses to let anyone write it.
TEST(RoadmapStoreIdentity, Inv3GeneratedFoldRejectsDirectWrite) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral(
        "INSERT INTO item (project_id, id, id_fold, id_origin, status, headline, "
        " kind, source) VALUES (?, 'Zz-1', 'WRONG', 'parsed', 'planned', 'h', "
        " 'implement', 'test')"));
    q.addBindValue(p);
    EXPECT_FALSE(q.exec())
        << "a writer must not be able to store an unfolded identity key";

    // And the generated value is the fold of what was actually written.
    ASSERT_TRUE(f.store.putItem(f.item(p, s, QStringLiteral("Sh-1"), 0), &err).has_value())
        << err.toStdString();
    QSqlQuery r(f.store.db());
    ASSERT_TRUE(r.exec(QStringLiteral("SELECT id, id_fold FROM item")));
    ASSERT_TRUE(r.next());
    EXPECT_EQ(r.value(0).toString().toStdString(), std::string("Sh-1"));
    EXPECT_EQ(r.value(1).toString().toStdString(), std::string("sh-1"));
}

// INV-4 — an off-grammar id is stored VERBATIM, quarantined, never rewritten.
TEST(RoadmapStoreIdentity, Inv4OffGrammarIdStoredVerbatim) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    const QString raw = QStringLiteral("[Cl9]");
    ASSERT_TRUE(f.store
                    .putItem(f.item(p, s, raw, 0, QStringLiteral("quarantined")), &err)
                    .has_value())
        << err.toStdString();

    QSqlQuery q(f.store.db());
    ASSERT_TRUE(q.exec(QStringLiteral("SELECT id, id_origin FROM item")));
    ASSERT_TRUE(q.next());
    EXPECT_EQ(q.value(0).toString().toStdString(), raw.toStdString())
        << "the authored spelling must round-trip exactly — no dash inserted, "
           "no bracket stripped";
    EXPECT_EQ(q.value(1).toString().toStdString(), std::string("quarantined"));
}

// INV-4 companion — id_origin has THREE values, not a boolean. A synthesised
// PASS-N-M id also fails the grammar but the model calls it a real id, so it
// must be storable as something other than 'quarantined'.
TEST(RoadmapStoreIdentity, Inv4SynthesisedIsDistinctFromQuarantined) {
    Fixture f;
    QString err;
    ASSERT_TRUE(f.open(&err)) << err.toStdString();
    const qint64 p = f.project(QStringLiteral("alpha"));
    const qint64 s = f.section(p);

    EXPECT_TRUE(f.store
                    .putItem(f.item(p, s, QStringLiteral("PASS-43-5-B"), 0,
                                    QStringLiteral("synthesised")),
                             &err)
                    .has_value())
        << err.toStdString();

    // And the enum is closed — a fourth value is refused (INV-11's id_origin leg).
    QSqlQuery q(f.store.db());
    q.prepare(QStringLiteral(
        "INSERT INTO item (project_id, id, id_origin, status, headline, kind, source) "
        "VALUES (?, 'X-1', 'guessed', 'planned', 'h', 'implement', 'test')"));
    q.addBindValue(p);
    EXPECT_FALSE(q.exec()) << "id_origin must be a closed enum at the storage layer";
}
