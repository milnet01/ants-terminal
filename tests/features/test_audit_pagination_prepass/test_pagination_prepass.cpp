// ANTS-2096 — paginated test_audit_partition must not strip pre-pass
// findings from the cached partition, so a later test_audit_brief on a
// page-2+ chunk still returns its pre_pass_findings.

#include <gtest/gtest.h>

#include "testauditengine.h"
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

#include <string>

namespace {

bool writeFile(const QString &path, const QString &body) {
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream ts(&f); ts << body;
    return true;
}

// A pytest project: pyproject.toml signal file + 8 test files, each
// carrying `time.sleep(` so the `sleep_call` pre-pass pattern hits every
// chunk. chunkSize=4 → two chunks (c-001, c-002), both with findings.
QString scaffoldPytestSuite(const QString &root) {
    writeFile(root + "/pyproject.toml",
              QStringLiteral("[tool.pytest.ini_options]\n"));
    for (int i = 1; i <= 8; ++i) {
        writeFile(root + QStringLiteral("/test_%1.py").arg(i, 3, 10, QLatin1Char('0')),
                  QStringLiteral("import time\n\n\ndef test_thing():\n"
                                 "    time.sleep(1)\n    assert True\n"));
    }
    return root;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — brief() on a page-2 chunk still returns its pre_pass_findings.
TEST(TestAuditPaginationPrePass, Inv1Page2BriefKeepsPrePass) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = scaffoldPytestSuite(tmp.path());

    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd  = root;
    preq.scope      = QStringLiteral("auto");
    preq.dimensions = QStringLiteral("auto");
    preq.chunkSize  = 4;
    preq.offset     = 1;   // page two: skip the first chunk
    preq.limit      = 1;   // one chunk per page

    const TestAuditEngine::PartitionResult p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << "partition failed: " << p.error.toStdString();
    ASSERT_EQ(p.framework, QStringLiteral("pytest"));
    ASSERT_EQ(p.chunks.size(), 1) << "offset=1,limit=1 should return one chunk";
    EXPECT_TRUE(p.prePassCached)
        << "page 2+ must flag prePassCached so the envelope omits the map";

    const QString page2ChunkId = p.chunks.first().id;

    TestAuditEngine::BriefRequest breq;
    breq.callerCwd      = root;
    breq.partitionToken = p.partitionToken;
    breq.chunkId        = page2ChunkId;

    const TestAuditEngine::BriefResult b = TestAuditEngine::brief(breq);
    ASSERT_TRUE(b.ok) << "brief failed: " << b.error.toStdString();
    // The regression: pre-fix this came back empty because partition()
    // cleared the cached map for page 2+.
    EXPECT_FALSE(b.prePassFindings.isEmpty())
        << "page-2 chunk " << page2ChunkId.toStdString()
        << " lost its pre_pass_findings (ANTS-2096 regression)";
}

// INV-2 — the envelope still omits the inline pre_pass map for a cached
// (page 2+) result, so ANTS-2070's token-saving holds. Source guard: the
// inline assignment must be gated on !r.prePassCached.
TEST(TestAuditPaginationPrePass, Inv2EnvelopeOmitsCachedMap) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_TRUE(contains(mw,
        "if (!prePassOmittedBySize && !r.prePassCached)"))
        << "INV-2: envelope must omit the inline pre_pass map when "
           "prePassCached (page 2+), not only on the size cap";
}

// ───────────────────────────────────────────────────────────────────
// ANTS-5126 — a partition run without the pre-pass. The dialog's panel
// refresh needs only the chunk set and the token, and the pre-pass (which
// reads and regex-scans every file in every chunk) is where partition's
// cost is: ~430 ms against the ~50 ms ANTS-1397 § 6 allows for a
// GUI-thread call. Skipping it must change nothing else.
// ───────────────────────────────────────────────────────────────────

// INV-3 — skipping the pre-pass yields the same chunk set and the same
// token. The dialog derives the token on the refresh path and dispatches
// against it later; a different token there would strand every brief.
TEST(TestAuditPaginationPrePass, Inv3NoPrePassKeepsChunksAndToken) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = scaffoldPytestSuite(tmp.path());

    TestAuditEngine::PartitionRequest base;
    base.callerCwd  = root;
    base.scope      = QStringLiteral("auto");
    base.dimensions = QStringLiteral("auto");
    base.chunkSize  = 4;

    const TestAuditEngine::PartitionResult withPre =
        TestAuditEngine::partition(base);
    ASSERT_TRUE(withPre.ok) << withPre.error.toStdString();

    TestAuditEngine::PartitionRequest lean = base;
    lean.prePass = false;
    const TestAuditEngine::PartitionResult noPre =
        TestAuditEngine::partition(lean);
    ASSERT_TRUE(noPre.ok) << noPre.error.toStdString();

    EXPECT_EQ(noPre.partitionToken, withPre.partitionToken)
        << "a pre-pass-free partition must derive the same token";
    ASSERT_EQ(noPre.chunks.size(), withPre.chunks.size())
        << "a pre-pass-free partition must produce the same chunk set";
    for (int i = 0; i < noPre.chunks.size(); ++i) {
        EXPECT_EQ(noPre.chunks[i].id, withPre.chunks[i].id);
        EXPECT_EQ(noPre.chunks[i].paths, withPre.chunks[i].paths);
    }
    EXPECT_EQ(noPre.framework, withPre.framework);
    EXPECT_TRUE(noPre.prePassFindingsByChunk.isEmpty())
        << "the pre-pass was asked to be skipped but ran anyway";
    EXPECT_FALSE(withPre.prePassFindingsByChunk.isEmpty())
        << "fixture no longer produces pre-pass findings — test is vacuous";
}

// INV-4 — a pre-pass-free partition seeds the cache but must not displace
// an entry already there. It has to seed: synthesis looks the partition up
// by token, and the dialog's panel refresh is what seeds it on open. It
// must not displace: test_audit_brief serves each chunk's findings out of
// this cache, so replacing a full entry with a lean one would empty every
// brief — the ANTS-2096 failure reached by another route.
TEST(TestAuditPaginationPrePass, Inv4NoPrePassDoesNotPoisonTheBriefCache) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = scaffoldPytestSuite(tmp.path());

    TestAuditEngine::PartitionRequest base;
    base.callerCwd  = root;
    base.scope      = QStringLiteral("auto");
    base.dimensions = QStringLiteral("auto");
    base.chunkSize  = 4;

    const TestAuditEngine::PartitionResult withPre =
        TestAuditEngine::partition(base);
    ASSERT_TRUE(withPre.ok) << withPre.error.toStdString();
    ASSERT_FALSE(withPre.chunks.isEmpty());
    const QString chunkId = withPre.chunks.first().id;

    // The panel refresh the dialog runs, after the dispatch path cached.
    TestAuditEngine::PartitionRequest lean = base;
    lean.prePass = false;
    const TestAuditEngine::PartitionResult noPre =
        TestAuditEngine::partition(lean);
    ASSERT_TRUE(noPre.ok) << noPre.error.toStdString();

    TestAuditEngine::BriefRequest breq;
    breq.callerCwd      = root;
    breq.partitionToken = withPre.partitionToken;
    breq.chunkId        = chunkId;

    const TestAuditEngine::BriefResult b = TestAuditEngine::brief(breq);
    ASSERT_TRUE(b.ok) << "brief failed: " << b.error.toStdString();
    EXPECT_FALSE(b.prePassFindings.isEmpty())
        << "a pre-pass-free partition displaced the cached one, so chunk "
        << chunkId.toStdString() << " lost its pre_pass_findings";
}
