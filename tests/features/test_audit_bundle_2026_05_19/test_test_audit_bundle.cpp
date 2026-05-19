// Feature-conformance test for the 2026-05-19 test_audit MCP fold-in
// bundle: ANTS-1615 (fold_in headline accept-or-refuse),
// ANTS-1616 (pre-pass regex widening to C/C++/Qt smells),
// ANTS-1617 (synthesise parser recognises heading-finding and
// `## Findings (JSON)` shapes).

#include <gtest/gtest.h>

#include "testauditengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

bool writeFile(const QString &path, const QString &body) {
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream ts(&f); ts << body;
    return true;
}

// Build a minimal ROADMAP + counter so foldIn() can write into a real
// project. The release-heading regex requires either a `(target: YYYY-N)`
// in-flight marker or a `\d+\.\d+\s*—` shipped heading.
void scaffoldRoadmap(const QString &root) {
    const QString rm =
        QStringLiteral("# Test ROADMAP\n\n"
                       "## 0.7.99 — bundle audit (target: 2026-05)\n\n"
                       "Existing body.\n");
    writeFile(root + "/ROADMAP.md", rm);
    writeFile(root + "/.roadmap-counter", QStringLiteral("100\n"));
}

QJsonObject mkActionable(const QString &headlineField,
                         const QString &headlineValue,
                         const QString &file = QStringLiteral("tests/x.cpp"),
                         int line = 42) {
    QJsonObject o;
    o["dimension"] = "flakiness";
    o["severity"]  = "HIGH";
    o["file"]      = file;
    o["line"]      = line;
    o["fix"]       = "Use a virtual clock";
    if (!headlineField.isEmpty()) o[headlineField] = headlineValue;
    return o;
}

}  // namespace

// ── ANTS-1615 ────────────────────────────────────────────────────────

TEST(TestAuditBundle201926519, Ants1615RefusesEmptyHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    scaffoldRoadmap(tmp.path());

    TestAuditEngine::FoldInRequest req;
    req.callerCwd = tmp.path();
    req.framework = "ctest";
    QJsonArray arr;
    arr.append(mkActionable(QString(), QString()));  // no headline field
    req.actionable = arr;

    const auto r = TestAuditEngine::foldIn(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_actionable"));
    EXPECT_TRUE(r.error.contains(QStringLiteral("headline")));
    EXPECT_TRUE(r.error.contains(QStringLiteral("summary")));
    EXPECT_TRUE(r.error.contains(QStringLiteral("claim")));
}

TEST(TestAuditBundle201926519, Ants1615AcceptsHeadlineField) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    scaffoldRoadmap(tmp.path());

    TestAuditEngine::FoldInRequest req;
    req.callerCwd = tmp.path();
    req.framework = "ctest";
    QJsonArray arr;
    arr.append(mkActionable("headline", "Sleep call in flaky test"));
    req.actionable = arr;

    const auto r = TestAuditEngine::foldIn(req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.block.contains(QStringLiteral("Sleep call in flaky test")));
    // No empty `**.**` rendering.
    EXPECT_FALSE(r.block.contains(QStringLiteral("**.**")));
}

TEST(TestAuditBundle201926519, Ants1615AcceptsSummaryFallback) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    scaffoldRoadmap(tmp.path());

    TestAuditEngine::FoldInRequest req;
    req.callerCwd = tmp.path();
    QJsonArray arr;
    arr.append(mkActionable("summary", "Summary text fallback"));
    req.actionable = arr;

    const auto r = TestAuditEngine::foldIn(req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.block.contains(QStringLiteral("Summary text fallback")));
}

TEST(TestAuditBundle201926519, Ants1615AcceptsClaimFallback) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    scaffoldRoadmap(tmp.path());

    TestAuditEngine::FoldInRequest req;
    req.callerCwd = tmp.path();
    QJsonArray arr;
    arr.append(mkActionable("claim", "Claim text fallback"));
    req.actionable = arr;

    const auto r = TestAuditEngine::foldIn(req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.block.contains(QStringLiteral("Claim text fallback")));
}

TEST(TestAuditBundle201926519, Ants1615TruncatesLongHeadline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    scaffoldRoadmap(tmp.path());

    TestAuditEngine::FoldInRequest req;
    req.callerCwd = tmp.path();
    QJsonArray arr;
    const QString longText = QString(200, QChar('A'));
    arr.append(mkActionable("headline", longText));
    req.actionable = arr;

    const auto r = TestAuditEngine::foldIn(req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    // The bullet should carry the truncated form with the ellipsis
    // marker, not the full 200-char headline.
    EXPECT_TRUE(r.block.contains(QStringLiteral(" \xE2\x80\xA6.**"))
                || r.block.contains(QString::fromUtf8(" \xE2\x80\xA6.**")))
        << "expected truncation suffix \" ….**\" in: "
        << r.block.toStdString();
}

// ── ANTS-1616 ────────────────────────────────────────────────────────
//
// The pre-pass pattern set must hit common C/C++/Qt test smells; v1
// was Python-only and produced a 0-hit rate. We exercise the public
// `partition` entry by laying down a tiny ctest fixture.

TEST(TestAuditBundle201926519, Ants1616PrePassHitsCppQtSmells) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Set up a ctest framework signal so detectFramework picks "ctest".
    writeFile(tmp.path() + "/CMakeLists.txt",
              QStringLiteral("project(t)\nenable_testing()\n"));
    // Test fixture pattern: ctest's test globs include
    // `tests/**/test_*.cpp` and `tests/**/test_*.py`.
    writeFile(tmp.path() + "/tests/test_smells.cpp",
              QStringLiteral(
                  "#include <thread>\n"
                  "#include <cstdlib>\n"
                  "#include <cstdio>\n"
                  "void f() {\n"
                  "    QThread::msleep(100);\n"
                  "    std::this_thread::sleep_for(std::chrono::seconds(1));\n"
                  "    std::exit(2);\n"
                  "    std::fprintf(stderr, \"FAIL: oops\\n\");\n"
                  "    qputenv(\"XDG_CONFIG_HOME\", \"/tmp\");\n"
                  "}\n"));

    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmp.path();
    preq.scope = QStringLiteral("auto");
    preq.dimensions = QStringLiteral("auto");
    preq.chunkSize = 12;

    const auto pr = TestAuditEngine::partition(preq);
    ASSERT_TRUE(pr.ok) << pr.error.toStdString();
    ASSERT_FALSE(pr.chunks.isEmpty());

    // Walk every chunk's findings, accumulating pattern_ids hit.
    QStringList hitPatternIds;
    for (const auto &c : pr.chunks) {
        const QJsonArray arr = pr.prePassFindingsByChunk.value(c.id);
        for (const QJsonValue &v : arr) {
            hitPatternIds.append(v.toObject()
                .value(QStringLiteral("pattern_id")).toString());
        }
    }
    // Expect at least these 5 from the widened set.
    EXPECT_TRUE(hitPatternIds.contains("qt_msleep"))
        << "hit ids: " << hitPatternIds.join(",").toStdString();
    EXPECT_TRUE(hitPatternIds.contains("cpp_sleep_for"));
    EXPECT_TRUE(hitPatternIds.contains("cpp_exit"));
    EXPECT_TRUE(hitPatternIds.contains("stderr_fail_print"));
    EXPECT_TRUE(hitPatternIds.contains("qputenv_call"));
}

// ── ANTS-1617 ────────────────────────────────────────────────────────

TEST(TestAuditBundle201926519, Ants1617SynthRecognisesHeadingFindingForm) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // synthesize() reads from a reports dir UNDER the project, so we
    // need both project root and reports subdir.
    const QString reportsDir = tmp.path() + "/reports";
    QDir().mkpath(reportsDir);

    // RetroDB convention: `### [SEV] dimension: <name> — <file>:<line>`.
    const QString c1 =
        QStringLiteral(
            "# c-001 report\n\n"
            "### [HIGH] dimension: flakiness — tests/a.cpp:12\n"
            "Body of finding 1.\n\n"
            "### [CRITICAL] dimension: security — tests/b.cpp:30\n"
            "Body of finding 2.\n\n"
            "### [MEDIUM] dimension: assertions — tests/c.cpp:8\n"
            "Body of finding 3.\n");
    writeFile(reportsDir + "/c-001.md", c1);

    TestAuditEngine::SynthRequest sreq;
    sreq.callerCwd = tmp.path();
    sreq.reportsDir = QStringLiteral("reports");
    sreq.mode = QStringLiteral("summary");
    // synthesize() requires a partitionToken from a partition() cache
    // hit. The pre-1617 path didn't gate on token presence for
    // reports-dir reads; we re-derive a token by calling partition()
    // first against the same caller_cwd.
    writeFile(tmp.path() + "/CMakeLists.txt",
              QStringLiteral("project(t)\nenable_testing()\n"));
    writeFile(tmp.path() + "/tests/test_a.cpp", QStringLiteral("int main(){}"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmp.path();
    preq.scope = QStringLiteral("auto");
    preq.dimensions = QStringLiteral("auto");
    const auto pr = TestAuditEngine::partition(preq);
    ASSERT_TRUE(pr.ok) << pr.error.toStdString();
    sreq.partitionToken = pr.partitionToken;

    const auto sr = TestAuditEngine::synthesize(sreq);
    ASSERT_TRUE(sr.ok) << sr.error.toStdString();
    // severity_histograms must now have flakiness/HIGH, security/CRIT,
    // assertions/MEDIUM filled in (was {} pre-1617).
    EXPECT_TRUE(sr.severityHistograms.contains("flakiness"));
    EXPECT_TRUE(sr.severityHistograms.contains("security"));
    EXPECT_TRUE(sr.severityHistograms.contains("assertions"));
    EXPECT_EQ(sr.severityHistograms.value("flakiness")
                  .toObject().value("high").toInt(), 1);
    EXPECT_EQ(sr.severityHistograms.value("security")
                  .toObject().value("crit").toInt(), 1);
    EXPECT_EQ(sr.severityHistograms.value("assertions")
                  .toObject().value("med").toInt(), 1);
}

TEST(TestAuditBundle201926519, Ants1617SynthRecognisesFindingsJsonBlock) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString reportsDir = tmp.path() + "/reports";
    QDir().mkpath(reportsDir);

    // 3D Engine convention: `## Findings (JSON)` + fenced JSON array.
    const QString c1 =
        QStringLiteral(
            "# c-001 report\n\n"
            "Some prose.\n\n"
            "## Findings (JSON)\n"
            "```json\n"
            "[\n"
            "  {\"severity\":\"HIGH\",\"dimension\":\"isolation\","
            "\"file\":\"tests/a.cpp\",\"line\":3},\n"
            "  {\"severity\":\"LOW\",\"dimension\":\"verbosity\","
            "\"file\":\"tests/b.cpp\",\"line\":7}\n"
            "]\n"
            "```\n");
    writeFile(reportsDir + "/c-001.md", c1);

    writeFile(tmp.path() + "/CMakeLists.txt",
              QStringLiteral("project(t)\nenable_testing()\n"));
    writeFile(tmp.path() + "/tests/test_a.cpp", QStringLiteral("int main(){}"));

    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmp.path();
    preq.scope = QStringLiteral("auto");
    preq.dimensions = QStringLiteral("auto");
    const auto pr = TestAuditEngine::partition(preq);
    ASSERT_TRUE(pr.ok) << pr.error.toStdString();

    TestAuditEngine::SynthRequest sreq;
    sreq.callerCwd = tmp.path();
    sreq.reportsDir = QStringLiteral("reports");
    sreq.mode = QStringLiteral("summary");
    sreq.partitionToken = pr.partitionToken;

    const auto sr = TestAuditEngine::synthesize(sreq);
    ASSERT_TRUE(sr.ok) << sr.error.toStdString();
    EXPECT_TRUE(sr.severityHistograms.contains("isolation"));
    EXPECT_TRUE(sr.severityHistograms.contains("verbosity"));
    EXPECT_EQ(sr.severityHistograms.value("isolation")
                  .toObject().value("high").toInt(), 1);
    EXPECT_EQ(sr.severityHistograms.value("verbosity")
                  .toObject().value("low").toInt(), 1);
}
