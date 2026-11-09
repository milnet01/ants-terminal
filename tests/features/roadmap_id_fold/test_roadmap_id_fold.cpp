// One id fold, matching the store's column — see spec.md. ANTS-5087.

#include "roadmapexport.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmapstore.h"
#include "../../_support/roadmapstoreaccess.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../src/") + QString::fromUtf8(rel);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Ä-1: the id the Mageia build folded differently (ANTS-5304).
QString accented() { return QString::fromUtf8("\xC3\x84-1"); }

// A real store in a temp dir, opened Bulk so a migration load and a rebuild
// may run on it too.
struct Store {
    QTemporaryDir dir;
    RoadmapStore store{dir.path() + QStringLiteral("/roadmap.sqlite"),
                       RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk};

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

    RoadmapStore::ItemWrite write(qint64 projectId, qint64 sectionId, const QString &id) {
        RoadmapStore::ItemWrite w;
        w.projectId = projectId;
        w.sectionId = sectionId;
        w.id = id;
        w.idOrigin = QStringLiteral("parsed");
        w.status = QStringLiteral("planned");
        w.headline = QStringLiteral("h");
        w.kind = QStringLiteral("implement");
        w.source = QStringLiteral("test");
        return w;
    }

    int rows(const char *table) {
        QSqlQuery q(RoadmapStoreTestAccess::db(store));
        if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM ") + QLatin1String(table)) || !q.next())
            return -1;
        return q.value(0).toInt();
    }
};

}  // namespace

// INV-1
//
// ANTS-5304 — the ids here are ASCII ON PURPOSE, and adding a non-ASCII one
// re-opens the defect this scope closed. `lower()` is not one function: a
// default SQLite folds only ASCII, and a build carrying the ICU extension
// (Mageia ships one) folds accented letters too. So a non-ASCII id compares
// foldId against the BUILD rather than against the contract, and the v0.7.110
// Mageia_10 package failed on exactly that while every openSUSE target passed.
// Above ASCII the two builds differ; at or below it they agree, so this
// comparison is the same on every one.
//
// It is also the whole domain: roadmap-format.md § 3.5 admits
// `[A-Za-z0-9_-]`-prefixed ids with a `-\d+` suffix, and a stable id is
// `^[A-Za-z][A-Za-z0-9_-]+$`. Neither can carry a character this test omits.
TEST(RoadmapIdFold, FoldMatchesSqliteLower) {
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("idfold"));
        db.setDatabaseName(QStringLiteral(":memory:"));
        ASSERT_TRUE(db.open());
        for (const QString &id : {QStringLiteral("ANTS-0042"), QStringLiteral("Sh4"),
                                  QStringLiteral("Ts20-SP6"), QStringLiteral("3D_E-0042"),
                                  QStringLiteral("MiXeD-7")}) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT lower(?)"));
            q.addBindValue(id);
            ASSERT_TRUE(q.exec() && q.next());
            EXPECT_EQ(RoadmapParse::foldId(id), q.value(0).toString())
                << "fold differs from SQLite lower() for " << id.toStdString();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("idfold"));
}

// INV-3 — foldId's own contract, asserted WITHOUT a database.
//
// The claim is about foldId and nothing else: it lowercases ASCII A-Z and
// leaves every other code point alone. Asserting that against SQLite would
// re-introduce the build dependence INV-1 just removed, so the expected values
// are written out here instead.
TEST(RoadmapIdFold, FoldLeavesNonAsciiUnchanged) {
    EXPECT_EQ(RoadmapParse::foldId(QString::fromUtf8("\xC3\x84-1")),
              QString::fromUtf8("\xC3\x84-1"));                     // Ä-1 → Ä-1
    EXPECT_EQ(RoadmapParse::foldId(QString::fromUtf8("\xC3\x89T\xC3\x89-7")),
              QString::fromUtf8("\xC3\x89t\xC3\x89-7"));            // ÉTÉ-7 → ÉtÉ-7
}

// INV-2
TEST(RoadmapIdFold, NoIdKeyUsesQtToLower) {
    static const QRegularExpression idLower(
        QStringLiteral(R"((\bid\b|IdFold|"id"\)\)\.toString\(\))[^;\n]*\.toLower\(\))"));
    for (const char *rel : {"roadmapexport.cpp", "roadmapmigrateload.cpp",
                            "roadmapmigrate.cpp", "roadmapstore.cpp"}) {
        const QString src = source(rel);
        ASSERT_FALSE(src.isEmpty()) << rel;
        const auto m = idLower.match(src);
        EXPECT_FALSE(m.hasMatch()) << rel << " keys an id with QString::toLower(): "
                                   << m.captured(0).toStdString();
    }
}

// INV-4 — the store refuses a non-ASCII id at every point that writes one, so
// foldId and the id_fold column cannot disagree on any SQLite build.
TEST(RoadmapIdFold, IsAsciiIdAcceptsOnlyAscii) {
    EXPECT_TRUE(RoadmapParse::isAsciiId(QStringLiteral("ANTS-0042")));
    EXPECT_TRUE(RoadmapParse::isAsciiId(QStringLiteral("[Cl9]")));   // off-grammar, ASCII
    EXPECT_TRUE(RoadmapParse::isAsciiId(QString()));
    EXPECT_TRUE(RoadmapParse::isAsciiId(QString(QChar(0x7F))));
    EXPECT_FALSE(RoadmapParse::isAsciiId(QString(QChar(0x80))));
    EXPECT_FALSE(RoadmapParse::isAsciiId(accented()));
}

TEST(RoadmapIdFold, StoreRefusesNonAsciiItemId) {
    Store s;
    QString err;
    ASSERT_TRUE(s.store.open(&err)) << err.toStdString();
    const qint64 p = s.project(QStringLiteral("alpha"));
    const qint64 sec = s.section(p);

    EXPECT_FALSE(s.store.putItem(s.write(p, sec, accented()), &err).has_value())
        << "putItem stored a non-ASCII id";
    EXPECT_TRUE(err.contains(QLatin1String("ASCII"))) << err.toStdString();
    EXPECT_EQ(s.rows("item"), 0);
    EXPECT_EQ(s.rows("element"), 0);
}

TEST(RoadmapIdFold, StoreRefusesNonAsciiReassignedId) {
    Store s;
    QString err;
    ASSERT_TRUE(s.store.open(&err)) << err.toStdString();
    const qint64 p = s.project(QStringLiteral("alpha"));
    const auto pk = s.store.putItem(s.write(p, s.section(p), QStringLiteral("ok-1")), &err);
    ASSERT_TRUE(pk.has_value()) << err.toStdString();

    err.clear();
    EXPECT_FALSE(s.store.reassignItemId(*pk, accented(), &err))
        << "reassignItemId wrote a non-ASCII id";
    EXPECT_TRUE(err.contains(QLatin1String("ASCII"))) << err.toStdString();
    QSqlQuery q(RoadmapStoreTestAccess::db(s.store));
    ASSERT_TRUE(q.exec(QStringLiteral("SELECT id FROM item")) && q.next());
    EXPECT_EQ(q.value(0).toString(), QStringLiteral("ok-1"));
}

TEST(RoadmapIdFold, StoreRefusesNonAsciiCrossProjectFold) {
    Store s;
    QString err;
    ASSERT_TRUE(s.store.open(&err)) << err.toStdString();
    const qint64 p = s.project(QStringLiteral("alpha"));
    const auto pk = s.store.putItem(s.write(p, s.section(p), QStringLiteral("ok-1")), &err);
    ASSERT_TRUE(pk.has_value()) << err.toStdString();

    err.clear();
    EXPECT_FALSE(s.store.relateCrossProject(QStringLiteral("blocked-by"), *pk,
                                            QStringLiteral("beta"), accented(), &err))
        << "relateCrossProject stored a non-ASCII dst_id_fold";
    EXPECT_TRUE(err.contains(QLatin1String("ASCII"))) << err.toStdString();
    EXPECT_EQ(s.rows("relationship"), 0);
}

// The rebuild reads an export file, which a hand edit or an older build may
// have written, so it checks the item id and a cross-project target itself.
TEST(RoadmapIdFold, RebuildRefusesNonAsciiIds) {
    // `which` is the ASCII id the export holds and the case replaces.
    for (const char *which : {"ok-1", "far-9"}) {
        Store src;
        QString err;
        ASSERT_TRUE(src.store.open(&err)) << err.toStdString();
        const qint64 p = src.project(QStringLiteral("alpha"));
        const auto pk = src.store.putItem(src.write(p, src.section(p), QStringLiteral("ok-1")),
                                          &err);
        ASSERT_TRUE(pk.has_value()) << err.toStdString();
        ASSERT_TRUE(src.store.relateCrossProject(QStringLiteral("blocked-by"), *pk,
                                                 QStringLiteral("beta"),
                                                 QStringLiteral("far-9"), &err))
            << err.toStdString();

        QByteArray bytes;
        {
            QBuffer out(&bytes);
            ASSERT_TRUE(out.open(QIODevice::WriteOnly));
            ASSERT_TRUE(RoadmapExport::writeProject(src.store, QStringLiteral("alpha"), &out, &err))
                << err.toStdString();
        }
        ASSERT_TRUE(bytes.contains(which)) << "precondition: the export names " << which;
        bytes.replace(QByteArray(which), accented().toUtf8());

        Store dst;
        ASSERT_TRUE(dst.store.open(&err)) << err.toStdString();
        QBuffer in(&bytes);
        ASSERT_TRUE(in.open(QIODevice::ReadOnly));
        err.clear();
        EXPECT_FALSE(RoadmapExport::rebuildProject(dst.store, &in, &err))
            << "the rebuild stored a non-ASCII id in place of " << which;
        EXPECT_TRUE(err.contains(QLatin1String("ASCII"))) << err.toStdString();
        EXPECT_EQ(dst.rows("item"), 0) << "a refused rebuild left rows (" << which << ")";
    }
}

// A bold lead-in that is prose becomes a quarantined id, so an accented one
// reaches the migration. It refuses the project and names the bullet, as a
// duplicate id does (roadmapmigrateload.cpp, matchItems), rather than
// dropping the label.
TEST(RoadmapIdFold, MigrationRefusesNonAsciiIdAndNamesTheBullet) {
    Store s;
    QString err;
    ASSERT_TRUE(s.store.open(&err)) << err.toStdString();
    const QString root = s.dir.path() + QStringLiteral("/proj");
    ASSERT_TRUE(QDir().mkpath(root));
    QByteArray md = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        md += "Padding to clear the minimum-parseable-size gate. \n";
    md += "\n## Systems\n\n"
          "- [ ] **A1** \xE2\x80\x94 An ASCII id.\n\n"
          "- [ ] **\xC3\x89t\xC3\xA9 mode** \xE2\x80\x94 an accented prose lead-in.\n\n";
    QFile f(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(md);
    f.close();

    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    ASSERT_TRUE(disc.has_value()) << err.toStdString();
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Proj"), QStringLiteral("proj"));
    bool sawAccented = false;
    for (const auto &it : plan.items)
        sawAccented |= !RoadmapParse::isAsciiId(it.id);
    ASSERT_TRUE(sawAccented) << "precondition: the plan carries the accented id";

    RoadmapMigrateLoad::Options opts;
    opts.changedAt = QStringLiteral("2026-11-09T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(s.store, plan, opts);
    EXPECT_FALSE(out.ok) << "the migration stored a non-ASCII id";
    EXPECT_TRUE(out.error.contains(QLatin1String("ASCII"))) << out.error.toStdString();
    EXPECT_TRUE(out.error.contains(QLatin1String("ROADMAP.md:"))) << out.error.toStdString();
    EXPECT_EQ(s.rows("item"), 0);
}
