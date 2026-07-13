// Feature-conformance test for ANTS-1286 ToolDetectionEngine.
// See tests/features/tool_detection_engine/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "tooldetectionengine.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>

#include <string>

#ifndef SRC_AUDITDIALOG_PATH
#error "SRC_AUDITDIALOG_PATH compile definition required"
#endif

namespace TDE = ToolDetectionEngine;

namespace {

// Restores PATH at scope-exit so test isolation holds.
struct PathScope {
    QByteArray saved;
    PathScope() : saved(qgetenv("PATH")) {}
    ~PathScope() { qputenv("PATH", saved); }
    void set(const QByteArray &v) { qputenv("PATH", v); }
};


}  // namespace

// TDE-1
TEST(ToolDetectionEngine, EmptyToolReturnsFalse) {
    TDE::clearCache();
    EXPECT_FALSE(TDE::exists(""));
    EXPECT_TRUE(TDE::resolve("").isEmpty());
    EXPECT_EQ(TDE::cacheSize(), 0) << "empty tool should not populate cache";
}

// TDE-2
TEST(ToolDetectionEngine, KnownToolResolvesAndCaches) {
    TDE::clearCache();
    const int before = TDE::cacheSize();
    const bool ok1 = TDE::exists("sh");
    EXPECT_TRUE(ok1) << "sh must be on PATH for this test";
    const int afterFirst = TDE::cacheSize();
    EXPECT_EQ(afterFirst, before + 1)
        << "first probe should add one cache entry";
    const bool ok2 = TDE::exists("sh");
    EXPECT_TRUE(ok2);
    EXPECT_EQ(TDE::cacheSize(), afterFirst)
        << "repeat probe must not grow the cache (INV-3)";
}

// TDE-3
TEST(ToolDetectionEngine, UnknownToolNegativeCaches) {
    TDE::clearCache();
    const QString fake = "ants-1286-fake-binary-nonsense";
    EXPECT_FALSE(TDE::exists(fake));
    const int afterFirst = TDE::cacheSize();
    EXPECT_EQ(afterFirst, 1) << "unknown tool should still cache the result";
    EXPECT_FALSE(TDE::exists(fake));
    EXPECT_EQ(TDE::cacheSize(), afterFirst)
        << "repeat unknown probe must not grow the cache";
}

// TDE-4
TEST(ToolDetectionEngine, PathChangeInvalidates) {
    PathScope scope;
    TDE::clearCache();
    const QString hBefore = TDE::currentPathHash();
    EXPECT_TRUE(TDE::exists("sh"));
    EXPECT_GE(TDE::cacheSize(), 1);
    // Mutate PATH to a path that excludes /usr/bin and /bin.
    scope.set("/dev/null");
    const QString hAfter = TDE::currentPathHash();
    EXPECT_NE(hBefore, hAfter) << "PATH change must change hash";
    // Next call sees the new hash and resets the cache.
    (void)TDE::exists("sh");
    EXPECT_LE(TDE::cacheSize(), 1)
        << "cache should have been reset on PATH change (INV-2)";
}

// TDE-5
TEST(ToolDetectionEngine, ResolveReturnsPath) {
    TDE::clearCache();
    const QString p = TDE::resolve("sh");
    EXPECT_FALSE(p.isEmpty()) << "sh should resolve to a non-empty path";
    EXPECT_TRUE(TDE::exists("sh"));
}

// TDE-6
TEST(ToolDetectionEngine, ClearCacheResets) {
    TDE::clearCache();
    (void)TDE::exists("sh");
    EXPECT_GE(TDE::cacheSize(), 1);
    TDE::clearCache();
    EXPECT_EQ(TDE::cacheSize(), 0) << "INV-11: clearCache must zero the cache";
    (void)TDE::exists("sh");
    EXPECT_GE(TDE::cacheSize(), 1) << "next call should repopulate";
}

// TDE-7 — INV-1: cache hits perform zero I/O.
TEST(ToolDetectionEngine, RepeatProbesAreFree) {
    TDE::clearCache();
    (void)TDE::exists("sh");  // warm
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 1000; ++i) {
        (void)TDE::exists("sh");
    }
    const qint64 elapsedMs = t.elapsed();
    // 200 ms ceiling, not 50 ms: 1000 cache hits are microseconds of real
    // work, but a loaded CI runner / sanitiser build can stretch the
    // wall-clock window well past 50 ms without any I/O actually
    // happening, flaking the INV-1 (zero-I/O) proxy (ANTS-1613).
    EXPECT_LT(elapsedMs, 200)
        << "1000 cache-hit probes should complete well under 200 ms; got "
        << elapsedMs << " ms (INV-1)";
}

// TDE-8 — INV-8 source-grep: AuditDialog::toolExists delegates.
TEST(ToolDetectionEngine, AuditDialogToolExistsDelegates) {
    const std::string ad = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(ad.empty());
    const auto pos = ad.find("bool AuditDialog::toolExists(");
    ASSERT_NE(pos, std::string::npos)
        << "AuditDialog::toolExists definition not found";
    const auto end = ad.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ad.substr(pos, end - pos);

    EXPECT_NE(body.find("ToolDetectionEngine::exists"), std::string::npos)
        << "INV-8: toolExists must delegate to ToolDetectionEngine::exists";
    EXPECT_EQ(body.find("QProcess"), std::string::npos)
        << "INV-7: no QProcess should remain in toolExists body";
    EXPECT_EQ(body.find("\"which\""), std::string::npos)
        << "INV-7: no \"which\" subprocess call should remain";
}
