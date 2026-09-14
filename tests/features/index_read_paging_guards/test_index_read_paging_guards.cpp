// read_region, pagination and index cache guards — see spec.md. ANTS-5103.

#include "paginationengine.h"
#include "readregion.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

// INV-1
TEST(IndexReadPagingGuards, OversizedFirstLineIsClipped) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("one-line.txt"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(200 * 1024, 'x'));
    }
    ReadRegion::Options o;
    o.hasLine   = true;
    o.startLine = 1;
    o.endLine   = 1;
    o.maxBytes  = 4096;
    const QJsonObject env = ReadRegion::extract(path, o);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    const QJsonArray lines = env.value(QStringLiteral("lines")).toArray();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_LE(lines.at(0).toString().toUtf8().size(), 4096 + 8)
        << "a single-line file came back whole past max_bytes";
    EXPECT_TRUE(env.value(QStringLiteral("truncated")).toBool());
}

// INV-2
TEST(IndexReadPagingGuards, AutoPageAlwaysAdvances) {
    QJsonArray rows;
    QJsonObject big;
    big[QStringLiteral("body")] = QString(PaginationEngine::kSoftCapBytes * 2, QLatin1Char('y'));
    rows.append(big);
    rows.append(QJsonObject{{QStringLiteral("body"), QStringLiteral("small")}});
    const PaginationEngine::PageResult r = PaginationEngine::pageBullets(rows, 0, -1, {});
    ASSERT_TRUE(r.truncated);
    EXPECT_GT(r.nextOffset, 0) << "next_offset equals offset, so a follower loops forever";
    EXPECT_EQ(r.slice.size(), 1);
}

// INV-3
TEST(IndexReadPagingGuards, CodebaseIndexChecksCacheCommit) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/codebaseindex.cpp"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
    EXPECT_TRUE(QString::fromUtf8(f.readAll()).contains(QStringLiteral("|| !sf.commit())")))
        << "codebase_index ignores QSaveFile::commit()'s result";
}
