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
TEST(RoadmapIdFold, FoldMatchesSqliteLower) {
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("idfold"));
        db.setDatabaseName(QStringLiteral(":memory:"));
        ASSERT_TRUE(db.open());
        for (const QString &id : {QStringLiteral("ANTS-0042"), QStringLiteral("Sh4"),
                                  QString::fromUtf8("\xC3\x84-1"),       // Ä-1
                                  QString::fromUtf8("\xC3\x89T\xC3\x89-7")}) {  // ÉTÉ-7
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
