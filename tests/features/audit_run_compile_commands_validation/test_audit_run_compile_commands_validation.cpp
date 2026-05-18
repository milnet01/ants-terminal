// Feature-conformance test for spec.md (ANTS-1446) —
// compile_commands.json include-path validation in audit_run.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "auditrunner.h"

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

}  // namespace

// ── INV-1 — extractIncludeArgs handles split + glued forms.

TEST(AuditRunCompileCommandsValidation, Inv1ExtractIncludeArgsSplitForm) {
    const QStringList args = {
        "-c", "src/foo.cpp",
        "-I", "/a/inc",
        "-isystem", "/b/inc",
        "-iquote", "/c/inc",
        "-include", "/d/file.h",
        "-O3"
    };
    const QStringList incl =
        AuditRunner::internal::extractIncludeArgs(args);
    ASSERT_EQ(incl.size(), 4);
    EXPECT_EQ(incl.at(0), QString("/a/inc"));
    EXPECT_EQ(incl.at(1), QString("/b/inc"));
    EXPECT_EQ(incl.at(2), QString("/c/inc"));
    EXPECT_EQ(incl.at(3), QString("/d/file.h"));
}

TEST(AuditRunCompileCommandsValidation, Inv1ExtractIncludeArgsGluedForm) {
    const QStringList args = {
        "-I/a/inc",
        "-isystem/b/inc",
        "-iquote/c/inc",
        "-include/d/file.h"
    };
    const QStringList incl =
        AuditRunner::internal::extractIncludeArgs(args);
    ASSERT_EQ(incl.size(), 4);
    EXPECT_EQ(incl.at(0), QString("/a/inc"));
    EXPECT_EQ(incl.at(1), QString("/b/inc"));
    EXPECT_EQ(incl.at(2), QString("/c/inc"));
    EXPECT_EQ(incl.at(3), QString("/d/file.h"));
}

// ── INV-2 — non-include flags are ignored.

TEST(AuditRunCompileCommandsValidation, Inv2ExtractIgnoresUnrelatedFlags) {
    const QStringList args = {
        "/usr/bin/c++", "-O3", "-std=c++20",
        "-DFOO=bar", "-Wall", "-c", "src/main.cpp",
        "-o", "main.o"
    };
    const QStringList incl =
        AuditRunner::internal::extractIncludeArgs(args);
    EXPECT_TRUE(incl.isEmpty());
}

// ── INV-3 — splitCommandString honours quoting + escapes.

TEST(AuditRunCompileCommandsValidation, Inv3SplitCommandStringBasic) {
    const QStringList out = AuditRunner::internal::splitCommandString(
        "cc -c foo.cpp -o foo.o");
    ASSERT_EQ(out.size(), 5);
    EXPECT_EQ(out.at(0), QString("cc"));
    EXPECT_EQ(out.at(4), QString("foo.o"));
}

TEST(AuditRunCompileCommandsValidation, Inv3SplitCommandStringDoubleQuotes) {
    const QStringList out = AuditRunner::internal::splitCommandString(
        "cc \"-DSTR=hello world\" -c f.cpp");
    ASSERT_EQ(out.size(), 4);
    EXPECT_EQ(out.at(1), QString("-DSTR=hello world"));
}

TEST(AuditRunCompileCommandsValidation, Inv3SplitCommandStringSingleQuotes) {
    const QStringList out = AuditRunner::internal::splitCommandString(
        "cc '-Dpath=/a b/c' -c f.cpp");
    ASSERT_EQ(out.size(), 4);
    EXPECT_EQ(out.at(1), QString("-Dpath=/a b/c"));
}

TEST(AuditRunCompileCommandsValidation, Inv3SplitCommandStringEscapes) {
    const QStringList out = AuditRunner::internal::splitCommandString(
        "cc -DSTR=\\\"hi\\\" -c f.cpp");
    ASSERT_EQ(out.size(), 4);
    EXPECT_EQ(out.at(1), QString("-DSTR=\"hi\""));
}

// ── INV-4 — project-root paths accepted.

TEST(AuditRunCompileCommandsValidation, Inv4ProjectRootAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir(tmp.path()).mkpath("include");

    QString reason;
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        tmp.path() + "/include", tmp.path(), tmp.path(), &reason))
        << "rejected reason: " << reason.toStdString();
    EXPECT_TRUE(reason.isEmpty());

    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "include", tmp.path(), tmp.path(), &reason));
}

// ── INV-5 — system prefix paths accepted.

TEST(AuditRunCompileCommandsValidation, Inv5SystemPrefixAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QString reason;
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "/usr/include/qt6", tmp.path(), tmp.path(), &reason));
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "/usr/lib64/qt6/mkspecs/linux-g++", tmp.path(), tmp.path(), &reason));
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "/opt/something", tmp.path(), tmp.path(), &reason));
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "/lib", tmp.path(), tmp.path(), &reason));
}

// ── INV-6 — out-of-tree non-system paths rejected.

TEST(AuditRunCompileCommandsValidation, Inv6OutOfTreePathRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QString reason;
    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        "/home/user/.ssh", tmp.path(), tmp.path(), &reason));
    EXPECT_FALSE(reason.isEmpty());
    EXPECT_TRUE(reason.contains("escapes project root"))
        << "reason was: " << reason.toStdString();

    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        "/tmp/secrets", tmp.path(), tmp.path(), &reason));
    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        "/var/lib/secret", tmp.path(), tmp.path(), &reason));
}

// ── INV-7 — control chars / backslashes refused.

TEST(AuditRunCompileCommandsValidation, Inv7ControlCharRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QString reason;
    QString bad = "/usr/include\x01/foo";  // SOH
    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        bad, tmp.path(), tmp.path(), &reason));
    EXPECT_TRUE(reason.contains("control char"));

    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        "\\foo", tmp.path(), tmp.path(), &reason));
    EXPECT_TRUE(reason.contains("control char")
             || reason.contains("backslash"));
}

// ── INV-8 — relative include resolves against entry dir.

TEST(AuditRunCompileCommandsValidation, Inv8RelativeResolvesAgainstEntryDir) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir(tmp.path()).mkpath("build/include");
    QDir(tmp.path()).mkpath("src/include");

    QString reason;
    // entryDir = build/, include = ./include → tmp/build/include ✓
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "include", tmp.path() + "/build", tmp.path(), &reason));

    // entryDir = build/, include = ../src/include → tmp/src/include ✓
    EXPECT_TRUE(AuditRunner::internal::isIncludePathAllowed(
        "../src/include", tmp.path() + "/build", tmp.path(), &reason));

    // entryDir = build/, include = ../../etc/passwd → /etc/passwd ✗
    EXPECT_FALSE(AuditRunner::internal::isIncludePathAllowed(
        "../../etc/passwd", tmp.path() + "/build", tmp.path(), &reason));
}

// ── INV-9 — absent JSON → no error (downstream handles).

TEST(AuditRunCompileCommandsValidation, Inv9AbsentJsonReturnsOk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QString reason;
    EXPECT_TRUE(AuditRunner::internal::validateCompileCommands(
        tmp.path(), &reason));
    EXPECT_TRUE(reason.isEmpty());
}

// ── INV-10 — clean JSON → accept.

TEST(AuditRunCompileCommandsValidation, Inv10CleanJsonAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir(tmp.path()).mkpath("build");
    QDir(tmp.path()).mkpath("src");

    const QString cwd = tmp.path();
    const QByteArray json = QByteArray(
        "[{"
            "\"directory\":\"") + cwd.toUtf8() + "/build\","
            "\"file\":\"" + cwd.toUtf8() + "/src/foo.cpp\","
            "\"arguments\":["
                "\"/usr/bin/c++\","
                "\"-I\",\"" + cwd.toUtf8() + "/src\","
                "\"-isystem\",\"/usr/include/qt6\","
                "\"-c\",\"" + cwd.toUtf8() + "/src/foo.cpp\""
            "]"
        "}]";
    writeFile(cwd, "build/compile_commands.json", json);

    QString reason;
    EXPECT_TRUE(AuditRunner::internal::validateCompileCommands(
        cwd, &reason))
        << "rejected: " << reason.toStdString();
    EXPECT_TRUE(reason.isEmpty());
}

// ── INV-11 — hostile include path → refuse + cite.

TEST(AuditRunCompileCommandsValidation, Inv11HostileIncludeRefused) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir(tmp.path()).mkpath("build");

    const QByteArray json =
        "[{"
            "\"directory\":\"" + tmp.path().toUtf8() + "/build\","
            "\"file\":\"" + tmp.path().toUtf8() + "/src/x.cpp\","
            "\"arguments\":["
                "\"/usr/bin/c++\","
                "\"-include\",\"/home/user/.ssh/id_rsa\","
                "\"-c\",\"src/x.cpp\""
            "]"
        "}]";
    writeFile(tmp.path(), "build/compile_commands.json", json);

    QString reason;
    EXPECT_FALSE(AuditRunner::internal::validateCompileCommands(
        tmp.path(), &reason));
    EXPECT_FALSE(reason.isEmpty());
    EXPECT_TRUE(reason.contains("id_rsa")
             || reason.contains("escapes project root"))
        << "reason was: " << reason.toStdString();
}

// ── INV-12 — `command` string form also walked.

TEST(AuditRunCompileCommandsValidation, Inv12CommandStringFormParsed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QDir(tmp.path()).mkpath("build");

    const QString cwd = tmp.path();
    const QByteArray json = QByteArray(
        "[{"
            "\"directory\":\"") + cwd.toUtf8() + "/build\","
            "\"file\":\"" + cwd.toUtf8() + "/src/foo.cpp\","
            "\"command\":\"/usr/bin/c++ -include /etc/shadow -c foo.cpp\""
        "}]";
    writeFile(cwd, "build/compile_commands.json", json);

    QString reason;
    EXPECT_FALSE(AuditRunner::internal::validateCompileCommands(
        cwd, &reason));
    EXPECT_TRUE(reason.contains("shadow")
             || reason.contains("escapes project root"))
        << "reason: " << reason.toStdString();
}

// ── INV-13 — runAudit wires the validator.

TEST(AuditRunCompileCommandsValidation, Inv13RunAuditWiresValidator) {
    const std::string src = slurp(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // validator is called before any spawn.
    const auto fn = src.find("runAudit(const RunRequest");
    ASSERT_NE(fn, std::string::npos);
    const std::string region = src.substr(fn, 8000);
    EXPECT_TRUE(contains(region, "validateCompileCommandsImpl"))
        << "runAudit must call validateCompileCommandsImpl before spawn";
    EXPECT_TRUE(contains(region, "compile_commands_escape"))
        << "refusal code on failure must be \"compile_commands_escape\"";
    EXPECT_TRUE(contains(region, "usesCompileCommands"))
        << "gate must check for clazy / clang-tidy presence";
}

// ── INV-14 — size caps in place.

TEST(AuditRunCompileCommandsValidation, Inv14SizeCapsDeclared) {
    const std::string src = slurp(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_TRUE(contains(src, "kCompileCommandsMaxBytes"));
    EXPECT_TRUE(contains(src, "kCompileCommandsMaxEntries"));
}
