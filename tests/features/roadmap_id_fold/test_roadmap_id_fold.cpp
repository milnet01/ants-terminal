// One id fold, matching the store's column — see spec.md. ANTS-5087.

#include "roadmapparse.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../src/") + QString::fromUtf8(rel);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

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
