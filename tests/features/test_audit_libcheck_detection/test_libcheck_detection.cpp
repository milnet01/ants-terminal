// ANTS-1624 — libcheck framework detection in test_audit_partition.
// Asserts INV-1..INV-6 from docs/specs/ANTS-1624.md.

#include "testauditengine.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace {

void touch(const QString &dir, const QString &relPath,
           const QByteArray &content = QByteArrayLiteral("// fixture\n")) {
    const QFileInfo fi(QDir(dir).filePath(relPath));
    QDir().mkpath(fi.absolutePath());
    QFile f(fi.absoluteFilePath());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

TestAuditEngine::PartitionResult partitionAt(const QString &root) {
    TestAuditEngine::PartitionRequest req;
    req.callerCwd = root;
    req.scope     = QStringLiteral("auto");
    req.dimensions = QStringLiteral("auto");
    return TestAuditEngine::partition(req);
}

}  // namespace

// INV-1 — probe-table entry exists.
TEST(TestAuditLibcheckDetection, Inv1ProbeEntryExists) {
    const std::string cpp = ants_test::slurpFile(SRC_TESTAUDITENGINE_CPP_PATH);
    EXPECT_TRUE(contains(cpp, "\"libcheck\""))
        << "INV-1: libcheck name literal must appear in g_kFrameworks";
    EXPECT_TRUE(contains(cpp, "\"Makefile.test\""))
        << "INV-1: Makefile.test signal-file literal must appear";
    EXPECT_TRUE(contains(cpp, "tests/**/test_*.c"))
        << "INV-1: tests/**/test_*.c glob must appear in libcheck entry";
}

// INV-2 — libcheck entry positioned before ctest in the probe table.
TEST(TestAuditLibcheckDetection, Inv2LibcheckBeforeCtest) {
    const std::string cpp = ants_test::slurpFile(SRC_TESTAUDITENGINE_CPP_PATH);
    const auto libPos = cpp.find("\"libcheck\"");
    const auto ctestPos = cpp.find("\"ctest\"");
    ASSERT_NE(libPos, std::string::npos);
    ASSERT_NE(ctestPos, std::string::npos);
    EXPECT_LT(libPos, ctestPos)
        << "INV-2: libcheck entry must appear before ctest entry so "
        << "Makefile.test wins over CMakeLists.txt on dual-build projects";
}

// INV-3 — bare Makefile.test + one C test file.
TEST(TestAuditLibcheckDetection, Inv3BareMakefileTest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "Makefile.test");
    touch(tmp.path(), "tests/test_hash.c",
          QByteArrayLiteral("#include <check.h>\n"
                            "START_TEST(test_hash) { ck_assert(1); }\n"
                            "END_TEST\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok) << "partition must succeed; code=" << r.code.toStdString()
                      << ", error=" << r.error.toStdString();
    EXPECT_EQ("libcheck", r.framework.toStdString());
    EXPECT_EQ(1, r.totalFiles);
}

// INV-4 — libretro-common-style layout (five test files).
TEST(TestAuditLibcheckDetection, Inv4LibretroCommonStyle) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "Makefile.test");
    const QStringList subs = {QStringLiteral("hash"), QStringLiteral("lists"),
                              QStringLiteral("queues"), QStringLiteral("string"),
                              QStringLiteral("utils")};
    for (const QString &s : subs) {
        touch(tmp.path(),
              QStringLiteral("libretro-common/test/%1/test_%1.c").arg(s),
              QByteArrayLiteral("#include <check.h>\n"));
    }
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok) << "partition must succeed; code=" << r.code.toStdString()
                      << ", error=" << r.error.toStdString();
    EXPECT_EQ("libcheck", r.framework.toStdString());
    EXPECT_EQ(5, r.totalFiles);
}

// INV-5 — CMakeLists.txt + Makefile.test → libcheck wins.
TEST(TestAuditLibcheckDetection, Inv5CMakeAndMakefileTestPrefersLibcheck) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "CMakeLists.txt");
    touch(tmp.path(), "Makefile.test");
    touch(tmp.path(), "tests/test_x.c",
          QByteArrayLiteral("#include <check.h>\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("libcheck", r.framework.toStdString())
        << "INV-5: libcheck must win over ctest when both signal files exist";
    EXPECT_EQ(1, r.totalFiles);
}

// INV-6 — pure-CMake unchanged (backwards-compat).
TEST(TestAuditLibcheckDetection, Inv6PureCMakeStillCtest) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    touch(tmp.path(), "CMakeLists.txt");
    touch(tmp.path(), "tests/test_x.cpp",
          QByteArrayLiteral("// ctest fixture\n"));
    const auto r = partitionAt(tmp.path());
    EXPECT_TRUE(r.ok);
    EXPECT_EQ("ctest", r.framework.toStdString())
        << "INV-6: pure-CMake projects must still detect ctest";
    EXPECT_EQ(1, r.totalFiles);
}
