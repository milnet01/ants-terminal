// ANTS-1451 — regression coverage for `walkTestFiles` build-tree
// exclusions. Fixture tree mirrors the live 2026-05-17 layout that
// surfaced moc_*.cpp / mocs_compilation.cpp under chunk c-001.

#include "testauditengine.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

namespace {

void touch(const QString &dir, const QString &relPath) {
    const QFileInfo fi(QDir(dir).filePath(relPath));
    QDir().mkpath(fi.absolutePath());
    QFile f(fi.absoluteFilePath());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("// fixture\n");
}

bool contains(const QStringList &xs, const QString &needle) {
    for (const QString &x : xs)
        if (x.contains(needle)) return true;
    return false;
}

QStringList ctestGlobs() {
    // Mirrors the ctest probe's testGlobs (see g_kFrameworks() in
    // src/testauditengine.cpp). If those change, this constant must
    // be updated in lockstep.
    return {QStringLiteral("tests/**/*.cpp"),
            QStringLiteral("test_*.cpp")};
}

}  // namespace

// INV-1 — every /build*/ preset is skipped.
TEST(TestAuditWalkExclusions, Inv1BuildPresetsExcluded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "build/test_a.cpp");
    touch(tmp.path(), "build-asan/test_b.cpp");
    touch(tmp.path(), "build-asan/moc_widget.cpp");
    touch(tmp.path(), "build-asan/test_claude_autogen/mocs_compilation.cpp");
    touch(tmp.path(), "build-workstation/test_c.cpp");
    touch(tmp.path(), "build-debug/test_d.cpp");
    touch(tmp.path(), "build-future-preset/test_e.cpp");
    touch(tmp.path(), "tests/test_real.cpp");

    const QStringList out = TestAuditEngine::internal::walkTestFiles(
        tmp.path(), ctestGlobs(), QStringLiteral("auto"));

    EXPECT_FALSE(contains(out, "/build/"));
    EXPECT_FALSE(contains(out, "/build-asan/"));
    EXPECT_FALSE(contains(out, "/build-workstation/"));
    EXPECT_FALSE(contains(out, "/build-debug/"));
    EXPECT_FALSE(contains(out, "/build-future-preset/"));
    EXPECT_FALSE(contains(out, "moc_widget.cpp"));
    EXPECT_FALSE(contains(out, "mocs_compilation.cpp"));
    EXPECT_TRUE(contains(out, "/tests/test_real.cpp"));
}

// INV-2 — CMake autogen / _deps / CMakeFiles subtrees are skipped.
TEST(TestAuditWalkExclusions, Inv2CMakeAutogenExcluded) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "_deps/googletest-src/test_gtest.cpp");
    touch(tmp.path(), "_deps/some_dep/tests/test_x.cpp");
    touch(tmp.path(), "CMakeFiles/test_y.cpp");
    touch(tmp.path(), "src/CMakeFiles/something_autogen/moc_foo.cpp");
    touch(tmp.path(), "out/autogen/include/moc_widget.cpp");
    touch(tmp.path(), "tests/test_real.cpp");

    const QStringList out = TestAuditEngine::internal::walkTestFiles(
        tmp.path(), ctestGlobs(), QStringLiteral("auto"));

    EXPECT_FALSE(contains(out, "/_deps/"));
    EXPECT_FALSE(contains(out, "/CMakeFiles/"));
    EXPECT_FALSE(contains(out, "/autogen/"));
    EXPECT_TRUE(contains(out, "/tests/test_real.cpp"));
}

// INV-3 — real test files surface despite a noisy build tree.
TEST(TestAuditWalkExclusions, Inv3RealTestsPreserved) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "build-asan/moc_a.cpp");
    touch(tmp.path(), "build-asan/moc_b.cpp");
    touch(tmp.path(), "tests/features/foo/test_foo.cpp");
    touch(tmp.path(), "tests/test_top_level.cpp");
    touch(tmp.path(), "test_basename_match.cpp");

    const QStringList out = TestAuditEngine::internal::walkTestFiles(
        tmp.path(), ctestGlobs(), QStringLiteral("auto"));

    EXPECT_TRUE(contains(out, "/tests/features/foo/test_foo.cpp"));
    EXPECT_TRUE(contains(out, "/tests/test_top_level.cpp"));
    EXPECT_TRUE(contains(out, "/test_basename_match.cpp"));
    // None of the build-tree noise.
    for (const QString &p : out) {
        EXPECT_FALSE(p.contains(QStringLiteral("/build-asan/")))
            << "unexpected: " << p.toStdString();
    }
}

// INV-4 — pre-existing exclusions retained (regression guard).
TEST(TestAuditWalkExclusions, Inv4LegacyExclusionsRetained) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "node_modules/foo/test_a.cpp");
    touch(tmp.path(), ".venv/lib/test_b.cpp");
    touch(tmp.path(), "src/__pycache__/test_c.cpp");
    touch(tmp.path(), "dist/test_d.cpp");
    touch(tmp.path(), "tests/test_real.cpp");

    const QStringList out = TestAuditEngine::internal::walkTestFiles(
        tmp.path(), ctestGlobs(), QStringLiteral("auto"));

    EXPECT_FALSE(contains(out, "/node_modules/"));
    EXPECT_FALSE(contains(out, "/.venv/"));
    EXPECT_FALSE(contains(out, "/__pycache__/"));
    EXPECT_FALSE(contains(out, "/dist/"));
    EXPECT_TRUE(contains(out, "/tests/test_real.cpp"));
}

// INV-5 — exclusion list is one compiled QRegularExpression in the
// engine source. Guards against future cleanup that reintroduces the
// chained-if pattern (each branch silently missing a preset).
TEST(TestAuditWalkExclusions, Inv5SingleRegexSourceOfTruth) {
    std::ifstream f(SRC_TESTAUDITENGINE_CPP_PATH);
    ASSERT_TRUE(f.good())
        << "cannot open " << SRC_TESTAUDITENGINE_CPP_PATH;
    std::stringstream ss; ss << f.rdbuf();
    const std::string cpp = ss.str();
    EXPECT_NE(cpp.find("static const QRegularExpression excludeRx"),
              std::string::npos)
        << "INV-5: exclusion list must be one static compiled regex";
    EXPECT_NE(cpp.find("build[^/]*"), std::string::npos)
        << "INV-5: /build*/ pattern in exclusion regex";
    EXPECT_NE(cpp.find("_deps"), std::string::npos)
        << "INV-5: _deps/ in exclusion regex";
    EXPECT_NE(cpp.find("CMakeFiles"), std::string::npos)
        << "INV-5: CMakeFiles/ in exclusion regex";
    EXPECT_NE(cpp.find("autogen"), std::string::npos)
        << "INV-5: autogen/ in exclusion regex";
    // The chained-if shape we're guarding against.
    EXPECT_EQ(cpp.find("p.contains(QLatin1String(\"/build/\"))"),
              std::string::npos)
        << "INV-5: old chained-if /build/ exclusion must not return";
}
