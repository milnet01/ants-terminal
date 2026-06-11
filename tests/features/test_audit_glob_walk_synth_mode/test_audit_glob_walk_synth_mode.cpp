// Feature-conformance test for ANTS-1455 — glob-walk + reports_dir
// opt-in + mode/pagination on the test_audit_* MCP trio.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "testauditengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>

#include <fstream>
#include <sstream>
#include <string>

#ifndef MCP_ERROR_CODES_DOC_PATH
#error "MCP_ERROR_CODES_DOC_PATH compile definition required"
#endif

namespace {

bool writeFile(const QString &path, const QString &body) {
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream ts(&f); ts << body;
    return true;
}


// Build a Flask-shaped project fixture per RetroDB's report:
//   app.py                  ← production code (must NOT be walked)
//   routes/scraper.py       ← production code (must NOT be walked)
//   services/jobs.py        ← production code (must NOT be walked)
//   tests/test_a.py         ← test (must be walked)
//   tests/unit/test_b.py    ← nested test (must be walked)
//   tests/conftest.py       ← pytest fixture file (must be walked
//                              because `test_*.py` doesn't match it
//                              and `tests/**/*.py` MUST match it
//                              under the new globToRegex)
//   test_root.py            ← bare-basename match (must be walked)
QString buildFlaskFixture(const QString &root) {
    EXPECT_TRUE(writeFile(root + "/app.py", "from flask import Flask\n"));
    EXPECT_TRUE(writeFile(root + "/routes/scraper.py",
                          "def run(): pass\n"));
    EXPECT_TRUE(writeFile(root + "/services/jobs.py", "x = 1\n"));
    EXPECT_TRUE(writeFile(root + "/tests/test_a.py",
                          "def test_a(): assert True\n"));
    EXPECT_TRUE(writeFile(root + "/tests/unit/test_b.py",
                          "def test_b(): assert True\n"));
    EXPECT_TRUE(writeFile(root + "/tests/conftest.py", "import pytest\n"));
    EXPECT_TRUE(writeFile(root + "/test_root.py",
                          "def test_root(): assert True\n"));
    return root;
}

const QStringList kPytestGlobs = {
    QStringLiteral("tests/**/*.py"),
    QStringLiteral("test_*.py"),
    QStringLiteral("*_test.py"),
};

}  // namespace

// ---------- G-1 / G-3a / G-15 — partition walk honours test_globs ---

TEST(TestAuditWalk, G1HonoursTestGlobs) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = buildFlaskFixture(tmp.path());
    const QStringList files =
        TestAuditEngine::internal::walkTestFiles(root, kPytestGlobs, QString());
    QSet<QString> rel;
    for (const QString &p : files) {
        rel.insert(p.mid(root.size() + 1));
    }
    // Must INCLUDE all four test files.
    EXPECT_TRUE(rel.contains("tests/test_a.py")) << rel.values().join(",").toStdString();
    EXPECT_TRUE(rel.contains("tests/unit/test_b.py"));
    EXPECT_TRUE(rel.contains("tests/conftest.py"));
    EXPECT_TRUE(rel.contains("test_root.py"));
    // Must EXCLUDE production source.
    EXPECT_FALSE(rel.contains("app.py")) << "app.py leaked into walk (RetroDB bug)";
    EXPECT_FALSE(rel.contains("routes/scraper.py"));
    EXPECT_FALSE(rel.contains("services/jobs.py"));
}

TEST(TestAuditWalk, G15BareBasenameMatchesAtAnyDepth) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    ASSERT_TRUE(writeFile(root + "/test_root.py", "x"));
    ASSERT_TRUE(writeFile(root + "/sub/test_sub.py", "x"));
    ASSERT_TRUE(writeFile(root + "/sub/deeper/test_d.py", "x"));
    const QStringList globs = {QStringLiteral("test_*.py")};
    const QStringList files =
        TestAuditEngine::internal::walkTestFiles(root, globs, QString());
    EXPECT_EQ(files.size(), 3);
}

// ---------- G-3 — scope="path:" / "files:" branches unaffected -----

TEST(TestAuditWalk, G3ScopePathBranch) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    const QString root = buildFlaskFixture(tmp.path());
    const QStringList files = TestAuditEngine::internal::walkTestFiles(
        root, kPytestGlobs, QStringLiteral("path:tests"));
    EXPECT_GT(files.size(), 0);
    for (const QString &p : files) {
        EXPECT_TRUE(p.contains("/tests/"))
            << "path:tests scope leaked outside tests/: " << p.toStdString();
    }
}

// ---------- G-4 — reports_dir default mode (no opt-in flag) --------

TEST(TestAuditSynth, G4OutsideRootRefusedByDefault) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    // Stage a partition so the synth gets past partition_token validation.
    // ctest-shape fixture so framework detection passes.
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();  // absolute path OUTSIDE the project
    // allow_outside_project defaults to false
    const auto r = TestAuditEngine::synthesize(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("reports_dir_outside_root"));
}

// ---------- G-5 — reports_dir with opt-in flag succeeds -----------

TEST(TestAuditSynth, G5OutsideRootAcceptedWithFlag) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    ASSERT_TRUE(writeFile(tmpReports.path() + "/c-001.md",
                          "## flakiness in test_a\n"));
    ASSERT_TRUE(writeFile(tmpReports.path() + "/c-002.md",
                          "## determinism issue\n"));

    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();
    req.allowOutsideProject = true;
    const auto r = TestAuditEngine::synthesize(req);
    EXPECT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
}

// ---------- G-6 — reports_dir_unreadable on bad absolute path -----

TEST(TestAuditSynth, G6UnreadableWithOptIn) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = QStringLiteral("/tmp/ants-1455-no-such-dir-12345");
    req.allowOutsideProject = true;
    const auto r = TestAuditEngine::synthesize(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("reports_dir_unreadable"));
}

// ---------- G-17 — reports_dir_empty on readable-but-empty -------

TEST(TestAuditSynth, G17EmptyDirRefused) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    // tmpReports is created empty — no *.md files
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();
    req.allowOutsideProject = true;
    const auto r = TestAuditEngine::synthesize(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("reports_dir_empty"));
}

// ---------- G-7 — mode arg validation ------------------------------

TEST(TestAuditSynth, G7BadModeRefused) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    ASSERT_TRUE(writeFile(tmpReports.path() + "/c-001.md", "x"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();
    req.allowOutsideProject = true;
    req.mode = QStringLiteral("foo");
    const auto r = TestAuditEngine::synthesize(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_mode"));
}

// ---------- G-8 — summary mode default + envelope shape ------------

TEST(TestAuditSynth, G8SummaryModeDefault) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(writeFile(tmpReports.path()
                              + QStringLiteral("/c-00%1.md").arg(i),
                              QStringLiteral("## flakiness finding\n"
                                             "app/file_%1.py:42 has issue\n")
                                  .arg(i)));
    }
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();
    req.allowOutsideProject = true;
    // mode omitted ⇒ default summary
    const auto r = TestAuditEngine::synthesize(req);
    ASSERT_TRUE(r.ok) << r.code.toStdString();
    EXPECT_EQ(r.mode, QStringLiteral("summary"));
    EXPECT_EQ(r.chunksTotal, 5);
    EXPECT_EQ(r.chunksReturned, 0);  // summary mode doesn't paginate
    EXPECT_EQ(r.nextOffset, -1);
    // Summary output ≤ 16 KiB.
    EXPECT_LE(r.byteCount, 16 * 1024);
    // Prompt is multi-line markdown — has at least a few `\n`s.
    EXPECT_GT(r.prompt.count(QChar('\n')), 5);
    // Top dimensions populated.
    EXPECT_GT(r.topDimensions.size(), 0);
    // file_index detected the file:line refs.
    EXPECT_GT(r.fileIndex.size(), 0);
}

// ---------- G-9 / G-14 — full mode pagination ----------------------

TEST(TestAuditSynth, G9FullModePaginates) {
    QTemporaryDir tmpProject; ASSERT_TRUE(tmpProject.isValid());
    QTemporaryDir tmpReports; ASSERT_TRUE(tmpReports.isValid());
    // 5 reports
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(writeFile(tmpReports.path()
                              + QStringLiteral("/c-00%1.md").arg(i),
                              QStringLiteral("chunk %1\n").arg(i)));
    }
    ASSERT_TRUE(writeFile(tmpProject.path() + "/CMakeLists.txt",
                          "project(t)\nenable_testing()\n"));
    ASSERT_TRUE(writeFile(tmpProject.path() + "/tests/test_x.cpp",
                          "int main() { return 0; }\n"));
    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmpProject.path();
    const auto p = TestAuditEngine::partition(preq);
    ASSERT_TRUE(p.ok) << p.error.toStdString();

    TestAuditEngine::SynthRequest req;
    req.callerCwd = tmpProject.path();
    req.partitionToken = p.partitionToken;
    req.reportsDir = tmpReports.path();
    req.allowOutsideProject = true;
    req.mode = QStringLiteral("full");
    req.limit = 2;
    req.offset = 0;
    const auto r1 = TestAuditEngine::synthesize(req);
    ASSERT_TRUE(r1.ok);
    EXPECT_EQ(r1.chunksTotal, 5);
    EXPECT_EQ(r1.chunksReturned, 2);
    EXPECT_EQ(r1.nextOffset, 2);
    EXPECT_TRUE(r1.truncatedByLimit);

    req.offset = 2;
    const auto r2 = TestAuditEngine::synthesize(req);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r2.chunksReturned, 2);
    EXPECT_EQ(r2.nextOffset, 4);

    req.offset = 4;
    const auto r3 = TestAuditEngine::synthesize(req);
    ASSERT_TRUE(r3.ok);
    EXPECT_EQ(r3.chunksReturned, 1);
    EXPECT_EQ(r3.nextOffset, -1);
    EXPECT_FALSE(r3.truncatedByLimit);
}

// ---------- G-13 — mcp-error-codes.md doc rows --------------------

TEST(TestAuditDocs, G13ErrorCodesPresentInTaxonomy) {
    const std::string doc = ants_test::slurpFile(MCP_ERROR_CODES_DOC_PATH);
    ASSERT_FALSE(doc.empty());
    EXPECT_NE(doc.find("reports_dir_outside_root"), std::string::npos);
    EXPECT_NE(doc.find("reports_dir_unreadable"), std::string::npos);
    EXPECT_NE(doc.find("reports_dir_empty"), std::string::npos);
}
