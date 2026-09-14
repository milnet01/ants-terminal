// Audit engine context reads and ledger cache — see spec.md.
// ANTS-5085. Source-scrape of src/auditengine.cpp and src/falseposledger.cpp.

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <gtest/gtest.h>

namespace {

QString source(const char *rel) {
    const QString path = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()
        + QStringLiteral("/../../../") + QString::fromUtf8(rel);
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll());
}

QString functionBody(const QString &src, const QString &signature) {
    const int start = src.indexOf(signature);
    if (start < 0) return QString();
    const int end = src.indexOf(QStringLiteral("\n}\n"), start);
    if (end < 0) return QString();
    return src.mid(start, end - start);
}

}  // namespace

// INV-1
TEST(AuditEngineLows, ContextFilterCapsFileReads) {
    const QString body = functionBody(source("src/auditengine.cpp"),
        QStringLiteral("FilterResult applyFilter("));
    ASSERT_FALSE(body.isEmpty()) << "applyFilter not found";
    const int cap = body.indexOf(QStringLiteral("<= kMaxContextFileBytes"));
    const int read = body.indexOf(QStringLiteral("src.readAll()"));
    ASSERT_GE(read, 0);
    ASSERT_GE(cap, 0) << "a referenced file of any size is read whole";
    EXPECT_LT(cap, read);
}

// INV-2
TEST(AuditEngineLows, LedgerCacheIsLocked) {
    const QString body = functionBody(source("src/falseposledger.cpp"),
        QStringLiteral("QList<LedgerEntry> loadEntries("));
    ASSERT_FALSE(body.isEmpty()) << "loadEntries not found";
    const int lock = body.indexOf(QStringLiteral("QMutexLocker"));
    const int find = body.indexOf(QStringLiteral("s_cache.find("));
    ASSERT_GE(find, 0);
    ASSERT_GE(lock, 0) << "the static ledger cache is reached from two threads unlocked";
    EXPECT_LT(lock, find);
}
