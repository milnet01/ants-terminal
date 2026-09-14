// Documentation engine fence rule and cache write — see spec.md. ANTS-5099.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    QFile f(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
            + QStringLiteral("/../../../src/") + QString::fromUtf8(rel));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-1
TEST(DocEngineGuards, TocWriterUsesSharedFenceRule) {
    const QString s = source("doclint.cpp");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_FALSE(s.contains(QStringLiteral("QVector<bool> fenceMap(")))
        << "doclint keeps a private fence scanner";
    EXPECT_TRUE(s.contains(QStringLiteral("const QVector<bool> fence = MarkdownScan::fenceMask(lines);")));
}

// INV-2
TEST(DocEngineGuards, DocsIndexChecksCacheCommit) {
    const QString s = source("docsindex.cpp");
    ASSERT_FALSE(s.isEmpty());
    EXPECT_TRUE(s.contains(QStringLiteral("|| !sf.commit())")))
        << "docs_index ignores QSaveFile::commit()'s result";
}
