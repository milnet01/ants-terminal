// Feature-conformance test for ANTS-5366 — a deregistered project keeps its
// row. Contract: tests/features/project_tombstone/spec.md, which points at
// docs/specs/ANTS-5366-deregistered-project-tombstone.md.
//
// NEVER default-construct RoadmapStore in a test: the default path resolves to
// the developer's REAL machine-global store.

#include <gtest/gtest.h>

#include "roadmapexport.h"
#include "roadmapmigrateverb.h"
#include "roadmapstore.h"
#include "../../_support/roadmapstoreaccess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariant>

#include <memory>

namespace {

constexpr const char *kT1    = "2026-10-01T00:00:00Z";
constexpr const char *kGone  = "2026-10-10T12:00:00Z";

struct Fixture {
    QTemporaryDir dir;
    std::unique_ptr<RoadmapStore> store;

    QString storePath() const { return dir.filePath(QStringLiteral("store.sqlite")); }

    bool init() {
        if (!dir.isValid())
            return false;
        return reopen();
    }
    bool reopen() {
        store = std::make_unique<RoadmapStore>(storePath());
        QString err;
        return store->open(&err);
    }
    void close() { store.reset(); }

    QString rootOf(const QString &slug) const {
        QDir().mkpath(dir.filePath(slug));
        return QFileInfo(dir.filePath(slug)).canonicalFilePath();
    }

    qint64 addProject(const QString &slug) {
        QString err;
        const auto pk = store->registerProject(rootOf(slug), slug, slug, &err);
        EXPECT_TRUE(pk.has_value()) << err.toStdString();
        return pk.value_or(0);
    }

    // One section holding one item, so the deregister has rows to delete.
    void addWork(qint64 projectId, const QString &itemId) {
        QString err;
        const auto s = store->addSection(projectId, QStringLiteral("work"),
                                         QStringLiteral("Work"), 2, 0,
                                         std::nullopt, &err);
        ASSERT_TRUE(s.has_value()) << err.toStdString();
        RoadmapStore::ItemWrite w;
        w.projectId = projectId;
        w.sectionId = *s;
        w.position  = 0;
        w.id        = itemId;
        w.status    = QStringLiteral("planned");
        w.headline  = QStringLiteral("An item.");
        w.kind      = QStringLiteral("implement");
        w.source    = QStringLiteral("test");
        ASSERT_TRUE(store->putItem(w, &err).has_value()) << err.toStdString();
    }

    bool deregister(qint64 projectId) {
        QString err;
        RoadmapStore::DeregisterCounts counts;
        const bool ok = store->deregisterProject(projectId, QString::fromLatin1(kGone),
                                                 &counts, &err);
        EXPECT_TRUE(ok) << err.toStdString();
        return ok;
    }

    int count(const QString &sql) {
        QSqlQuery q(RoadmapStoreTestAccess::db(*store));
        if (!q.exec(sql) || !q.next())
            return -1;
        return q.value(0).toInt();
    }

    bool send(qint64 from, const QString &toSlug, const QString &body) {
        QString code, err;
        qint64 id = 0;
        return store->sendMessage(from, toSlug, body, QString(),
                                  QString::fromLatin1(kT1), &id, &code, &err);
    }
};

using V = RoadmapStore::Visibility;

}  // namespace

// INV-1 — the row stays, stamped; the project's roadmap rows go.
TEST(ProjectTombstone, Inv1DeregisterKeepsTheRowAndClearsTheRoadmap) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    f.addWork(b, QStringLiteral("B-1"));
    ASSERT_TRUE(f.deregister(b));

    QString err;
    const auto row = f.store->readProject(b, &err, V::IncludeDeregistered);
    ASSERT_TRUE(row.has_value()) << "INV-1: the project row was deleted";
    EXPECT_EQ(row->deregisteredAt, QString::fromLatin1(kGone));
    EXPECT_EQ(f.count(QStringLiteral("SELECT count(*) FROM item WHERE project_id = %1").arg(b)), 0);
    EXPECT_EQ(f.count(QStringLiteral("SELECT count(*) FROM section WHERE project_id = %1").arg(b)), 0);
}

// INV-2 — mail survives at both ends.
TEST(ProjectTombstone, Inv2DeregisterDeletesNoMessage) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.send(a, QStringLiteral("bravo"), QStringLiteral("a->b")));
    ASSERT_TRUE(f.send(b, QStringLiteral("alpha"), QStringLiteral("b->a")));
    ASSERT_TRUE(f.deregister(b));
    EXPECT_EQ(f.count(QStringLiteral("SELECT count(*) FROM message")), 2)
        << "INV-2: deregistering deleted mail";
}

// INV-3 — the departed project still has a mailbox.
TEST(ProjectTombstone, Inv3MailReachesADeregisteredProject) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.deregister(b));

    EXPECT_TRUE(f.send(a, QStringLiteral("bravo"), QStringLiteral("news")))
        << "INV-3: mail to a departed slug was refused";

    QString err;
    const auto self = f.store->projectIdForRoot(f.rootOf(QStringLiteral("bravo")), &err);
    ASSERT_TRUE(self.has_value()) << "INV-3: the inbox cannot resolve its own root";
    EXPECT_EQ(*self, b);
    QVector<RoadmapStore::Message> got;
    ASSERT_TRUE(f.store->inboxFor(*self, false, 0, 0, &got, nullptr, &err)) << err.toStdString();
    ASSERT_EQ(got.size(), 1);
    EXPECT_EQ(got[0].body, QStringLiteral("news"));

    EXPECT_TRUE(f.store->slugCandidates(QStringLiteral("Bravo")).contains(QStringLiteral("bravo")))
        << "INV-3: a near miss does not offer the departed slug";
}

// INV-4 — the roadmap readers hide the row unless asked.
TEST(ProjectTombstone, Inv4RoadmapReadersHideTheRow) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 a = f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.deregister(b));
    const QString rootB = f.rootOf(QStringLiteral("bravo"));

    EXPECT_FALSE(f.store->readProject(b).has_value());
    EXPECT_FALSE(f.store->readProjectBySlug(QStringLiteral("bravo")).has_value());
    EXPECT_FALSE(f.store->readProjectByRoot(rootB).has_value());
    const auto listed = f.store->listProjects();
    ASSERT_EQ(listed.size(), 1);
    EXPECT_EQ(listed[0].projectId, a) << "INV-4: the live sibling must still be returned";
    EXPECT_TRUE(f.store->readProject(a).has_value());

    EXPECT_TRUE(f.store->readProjectBySlug(QStringLiteral("bravo"), nullptr,
                                           V::IncludeDeregistered).has_value());
    EXPECT_EQ(f.store->listProjects(nullptr, V::IncludeDeregistered).size(), 2);
}

// INV-5 — the highest id is not handed on (ANTS-5483).
TEST(ProjectTombstone, Inv5ProjectIdIsNeverReused) {
    Fixture f;
    ASSERT_TRUE(f.init());
    f.addProject(QStringLiteral("alpha"));
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.deregister(b));
    const qint64 c = f.addProject(QStringLiteral("charlie"));
    EXPECT_GT(c, b) << "INV-5: SQLite reused the departed project's id";
}

// INV-6 — registering the root again revives the same row.
TEST(ProjectTombstone, Inv6RegisteringTheRootRevivesTheRow) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.deregister(b));
    EXPECT_EQ(f.addProject(QStringLiteral("bravo")), b);
    const auto row = f.store->readProject(b);
    ASSERT_TRUE(row.has_value()) << "INV-6: the revived row is still hidden";
    EXPECT_TRUE(row->deregisteredAt.isEmpty());
}

// INV-7 — a departed slug stays taken, through the verb.
TEST(ProjectTombstone, Inv7DepartedSlugStaysTaken) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    ASSERT_TRUE(f.deregister(b));
    f.close();

    const QString newRoot = f.rootOf(QStringLiteral("newcomer"));
    {
        QFile md(newRoot + QStringLiteral("/ROADMAP.md"));
        ASSERT_TRUE(md.open(QIODevice::WriteOnly));
        md.write("<!-- ants-roadmap-format: 1 -->\n\n# New — Roadmap\n\n## Work\n\n"
                 "- \xF0\x9F\x93\x8B [NEW-0001] **An item.**\n"
                 "  Layman: A thing.\n  Kind: implement.\n  Source: test.\n");
    }
    RoadmapMigrateVerb::Request r;
    r.projectRoot = newRoot;
    r.projectName = QStringLiteral("Newcomer");
    r.exportSlug  = QStringLiteral("bravo");
    r.changedAt   = QString::fromLatin1(kGone);
    const QJsonObject env = RoadmapMigrateVerb::run(f.storePath(), r);

    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(), QStringLiteral("slug_collision"))
        << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(env.value(QStringLiteral("deregistered")).toBool());

    ASSERT_TRUE(f.reopen());
    EXPECT_EQ(f.count(QStringLiteral("SELECT count(*) FROM project")), 1)
        << "INV-7: the refused migrate wrote a project row";
}

// INV-8 — restoring a project's own export at its own root revives the row.
TEST(ProjectTombstone, Inv8RestoreOntoItsOwnDeregisteredRow) {
    Fixture f;
    ASSERT_TRUE(f.init());
    const qint64 b = f.addProject(QStringLiteral("bravo"));
    f.addWork(b, QStringLiteral("B-1"));
    const QString exportPath = f.dir.filePath(QStringLiteral("bravo.jsonl"));
    QString err;
    ASSERT_TRUE(RoadmapExport::exportProject(*f.store, QStringLiteral("bravo"),
                                             exportPath, &err)) << err.toStdString();
    ASSERT_TRUE(f.deregister(b));
    f.close();

    QString printed;
    QTextStream ts(&printed);
    const int rc = RoadmapExport::runImportCommand(
        f.storePath(), exportPath, f.rootOf(QStringLiteral("bravo")), ts);
    ts.flush();
    ASSERT_EQ(rc, 0) << printed.toStdString();

    ASSERT_TRUE(f.reopen());
    const auto row = f.store->readProjectBySlug(QStringLiteral("bravo"));
    ASSERT_TRUE(row.has_value()) << "INV-8: the restored project is not registered";
    EXPECT_EQ(row->projectId, b) << "INV-8: the restore did not reuse its own row";
    EXPECT_EQ(f.count(QStringLiteral("SELECT count(*) FROM item WHERE project_id = %1").arg(b)), 1)
        << "INV-8: the items did not come back";
}
