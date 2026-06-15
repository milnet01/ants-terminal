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
