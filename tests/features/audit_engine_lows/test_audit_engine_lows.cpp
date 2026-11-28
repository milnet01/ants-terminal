// Audit engine context reads and ledger cache — see spec.md.
// ANTS-5085. Source-scrape of src/auditengine.cpp and src/falseposledger.cpp.
// ANTS-6622 adds behavioural checks (INV-3..5) against the engine itself.

#include "auditengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

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
    const QString src = source("src/auditengine.cpp");
    const QString filter = functionBody(src,
        QStringLiteral("FilterResult applyFilter("));
    ASSERT_FALSE(filter.isEmpty()) << "applyFilter not found";
    EXPECT_TRUE(filter.contains(QStringLiteral("contextFileLines(")))
        << "applyFilter reads context files through contextFileLines";
    const QString body = functionBody(src,
        QStringLiteral("const QStringList &contextFileLines("));
    ASSERT_FALSE(body.isEmpty()) << "contextFileLines not found";
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

// INV-3 (ANTS-6622) — `test_` marks a test path only at a segment start.
TEST(AuditEngineLows, TestPathPenaltyNeedsSegmentStart) {
    Finding f;
    f.source = QStringLiteral("cppcheck");
    f.file = QStringLiteral("src/latest_build.cpp");
    const int plain = AuditEngine::computeConfidence(f);
    f.file = QStringLiteral("src/test_build.cpp");
    const int test = AuditEngine::computeConfidence(f);
    EXPECT_EQ(plain - test, 20) << "latest_*.cpp was scored as a test path";
    f.file = QStringLiteral("test_build.cpp");
    EXPECT_EQ(AuditEngine::computeConfidence(f), test);
}

// INV-6 (ANTS-6982) — a relative path under tests/ or test/ is a test path.
TEST(AuditEngineLows, TestDirPenaltyNeedsNoLeadingSlash) {
    Finding f;
    f.source = QStringLiteral("cppcheck");
    f.file = QStringLiteral("src/foo.cpp");
    const int plain = AuditEngine::computeConfidence(f);
    for (const char *path : {"tests/foo.cpp", "test/foo.cpp"}) {
        f.file = QString::fromLatin1(path);
        EXPECT_EQ(plain - AuditEngine::computeConfidence(f), 20)
            << path << " was not scored as a test path";
    }
}

namespace {
bool writeText(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(body) == body.size();
}
}  // namespace

// INV-4 (ANTS-6622) — a clang-tidy finding in a path with a space is kept.
TEST(AuditEngineLows, ClangTidyKeepsSpacedPath) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString txt = dir.filePath(QStringLiteral("tidy.txt"));
    ASSERT_TRUE(writeText(txt,
        "/p/My Project/a.cpp:3:5: warning: bad thing [check-x]\n"
        "2 warnings generated.\n"));
    const auto s = AuditEngine::summariseClangTidyText(txt, 10, QStringLiteral("note"));
    ASSERT_TRUE(s.has_value()) << "the only finding was dropped";
    ASSERT_EQ(s->topFindings.size(), 1);
    EXPECT_EQ(s->topFindings.first().file, QStringLiteral("/p/My Project/a.cpp"));
}

// INV-5 (ANTS-6622) — the sibling HTML is derived only from a `.sarif` name.
TEST(AuditEngineLows, HtmlSiblingNeedsSarifSuffix) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sarif = dir.filePath(QStringLiteral("r.json"));
    ASSERT_TRUE(writeText(sarif, R"({"version":"2.1.0","runs":[{"results":[]}]})"));
    // chop(6) on "<dir>/r.json" left "<dir>/", so ".html" was taken as its page.
    ASSERT_TRUE(writeText(dir.filePath(QStringLiteral(".html")), "<html/>"));
    const auto s = AuditEngine::summariseSarif(sarif, 10, QStringLiteral("note"));
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->htmlPath.isEmpty()) << s->htmlPath.toStdString();
}
