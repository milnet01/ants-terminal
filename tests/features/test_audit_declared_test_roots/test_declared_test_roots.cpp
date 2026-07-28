// ANTS-3708 — feature-conformance test: test_audit_partition treats a
// human declaration (.ants/project.json test_roots, or an explicit
// scope:"files:") as outranking a failed framework sniff, instead of
// refusing no_tests_found. See
// tests/features/test_audit_declared_test_roots/spec.md.

#include <gtest/gtest.h>

#include "testauditengine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &root, const QString &rel, const QByteArray &body) {
    const QString abs = QDir(root).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

// A hand-rolled C harness: no ctest, no Catch2, no gtest, no signal file
// of any kind — the DOOM Ants shape.
QString seedHandRolled(QTemporaryDir &tmp, bool declareTestRoots) {
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    writeFile(root, QStringLiteral("linuxdoom-1.10/m_misc.c"), "int x;\n");
    writeFile(root, QStringLiteral("linuxdoom-1.10/tests/mus2mid_test.c"),
              "int main(void) { return 0; }\n");
    writeFile(root, QStringLiteral("linuxdoom-1.10/tests/wad_test.c"),
              "int main(void) { return 0; }\n");
    if (declareTestRoots) {
        writeFile(root, QStringLiteral(".ants/project.json"),
                  "{\"source_roots\":[\"linuxdoom-1.10\"],"
                  " \"test_roots\":[\"linuxdoom-1.10/tests\"]}\n");
    }
    return root;
}

QStringList allChunkPaths(const TestAuditEngine::PartitionResult &r) {
    QStringList out;
    for (const auto &c : r.chunks) out += c.paths;
    return out;
}

}  // namespace

// INV-1 — a declared test_roots rescues a failed sniff.
TEST(TestAuditDeclaredTestRoots, Inv1DeclaredTestRootsRescueFailedSniff) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedHandRolled(tmp, /*declareTestRoots=*/true);

    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    const auto r = TestAuditEngine::partition(req);

    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    EXPECT_EQ(r.framework, QStringLiteral("custom"));
    EXPECT_EQ(r.totalFiles, 2);
    const QStringList paths = allChunkPaths(r);
    EXPECT_TRUE(paths.filter(QStringLiteral("mus2mid_test.c")).size() == 1)
        << paths.join(QStringLiteral(",")).toStdString();
    // The declared root bounds the walk — production source stays out.
    EXPECT_TRUE(paths.filter(QStringLiteral("m_misc.c")).isEmpty());
}

// INV-2 — an explicit files: scope needs no framework at all.
TEST(TestAuditDeclaredTestRoots, Inv2ExplicitFilesScopeNeedsNoFramework) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedHandRolled(tmp, /*declareTestRoots=*/false);

    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    req.scope = QStringLiteral(
        "files:linuxdoom-1.10/tests/mus2mid_test.c,"
        "linuxdoom-1.10/tests/wad_test.c");
    const auto r = TestAuditEngine::partition(req);

    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    EXPECT_EQ(r.framework, QStringLiteral("custom"));
    EXPECT_EQ(r.totalFiles, 2);
}

// INV-3 — a successfully detected framework is not overridden.
TEST(TestAuditDeclaredTestRoots, Inv3DetectedFrameworkNotOverridden) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    writeFile(root, QStringLiteral("CMakeLists.txt"), "project(t)\n");
    writeFile(root, QStringLiteral("tests/test_a.cpp"), "int main(){return 0;}\n");
    writeFile(root, QStringLiteral(".ants/project.json"),
              "{\"test_roots\":[\"tests\"]}\n");

    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    const auto r = TestAuditEngine::partition(req);

    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    EXPECT_EQ(r.framework, QStringLiteral("ctest"))
        << "a declaration must rescue a failed sniff, not override a good one";
}

// INV-4 — declared roots also rescue a framework whose globs match nothing.
TEST(TestAuditDeclaredTestRoots, Inv4DeclaredRootsRescueZeroFileWalk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    // ctest is detected at the root, but its globs (tests/**/*.cpp,
    // test_*.cpp) match nothing — the suite is hand-rolled C, elsewhere.
    writeFile(root, QStringLiteral("CMakeLists.txt"), "project(t)\n");
    writeFile(root, QStringLiteral("suite/checks/wad_check.c"),
              "int main(void){return 0;}\n");
    writeFile(root, QStringLiteral(".ants/project.json"),
              "{\"test_roots\":[\"suite/checks\"]}\n");

    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    const auto r = TestAuditEngine::partition(req);

    ASSERT_TRUE(r.ok) << r.code.toStdString() << ": " << r.error.toStdString();
    EXPECT_EQ(r.framework, QStringLiteral("ctest"))
        << "the signal file really is there — only the globs were wrong";
    EXPECT_EQ(r.totalFiles, 1);
}

// INV-5 — with neither a signal file nor a declaration, the refusal says
// what was probed and how to answer it.
TEST(TestAuditDeclaredTestRoots, Inv5RefusalNamesWhatWasProbed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedHandRolled(tmp, /*declareTestRoots=*/false);

    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    const auto r = TestAuditEngine::partition(req);

    ASSERT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("no_tests_found"));
    const std::string err = r.error.toStdString();
    EXPECT_NE(err.find("CMakeLists.txt"), std::string::npos) << err;
    EXPECT_NE(err.find("pyproject.toml"), std::string::npos) << err;
    EXPECT_NE(err.find("test_roots"), std::string::npos) << err;
    EXPECT_NE(err.find("files:"), std::string::npos) << err;
}
