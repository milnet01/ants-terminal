// Feature-conformance test for spec.md (ANTS-6520).

#include "auditlinecache.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

namespace {
QString writeLines(const QTemporaryDir &dir, const QString &name, int lines, int width) {
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    const QByteArray line = QByteArray(width - 1, 'a') + '\n';
    for (int i = 0; i < lines; ++i) f.write(line);
    return path;
}
}  // namespace

// INV-1
TEST(AuditLineCache, ReadsAFileOnce) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString p = writeLines(tmp, QStringLiteral("a.txt"), 3, 10);
    AuditLineCache cache;
    const QStringList first = cache.lines(p);
    ASSERT_TRUE(QFile::remove(p));
    EXPECT_EQ(cache.lines(p), first) << "the second read went back to disk";
    EXPECT_EQ(first.size(), 4);   // three lines and the empty tail
}

// INV-2
TEST(AuditLineCache, HoldsAtMostItsCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    AuditLineCache cache;
    QString last;
    for (int i = 0; i < 12; ++i) {   // 12 × 3 MB = 36 M characters
        last = writeLines(tmp, QStringLiteral("f%1.txt").arg(i), 30000, 100);
        ASSERT_FALSE(last.isEmpty());
        ASSERT_EQ(cache.lines(last).size(), 30001);
        EXPECT_LE(cache.cachedChars(), AuditLineCache::kMaxChars)
            << "after file " << i << " the cache holds " << cache.cachedChars();
    }
    const qsizetype held = cache.cachedChars();
    EXPECT_GT(held, 0) << "the file just read was not kept";
    ASSERT_TRUE(QFile::remove(last));
    EXPECT_EQ(cache.lines(last).size(), 30001) << "the file just read was not kept";
    EXPECT_EQ(cache.cachedChars(), held);
}

// INV-3
TEST(AuditLineCache, LargeAndMissingFiles) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString big = writeLines(tmp, QStringLiteral("big.txt"),
                                   int(AuditLineCache::kMaxFileBytes / 64), 64);
    AuditLineCache cache;
    EXPECT_EQ(cache.lines(big), QStringList{QString()});
    EXPECT_TRUE(cache.lines(tmp.filePath(QStringLiteral("absent.txt"))).isEmpty());
}
