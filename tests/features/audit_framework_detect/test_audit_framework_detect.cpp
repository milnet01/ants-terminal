// Feature-conformance test for ANTS-1111 — AuditHygiene framework
// auto-detect.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "audithygiene.h"

namespace {

void writeFile(const QString &dir, const QString &name,
               const QByteArray &body) {
    QFile f(QDir(dir).filePath(name));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

}  // namespace

TEST(AuditFrameworkDetect, Inv4FlaskFromRequirements) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "requirements.txt",
              "flask>=2.0\nrequests==2.31.0\n");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("flask"));
}

TEST(AuditFrameworkDetect, Inv4ReactFromPackageJson) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "package.json",
              "{ \"dependencies\": { \"react\": \"^18.0.0\" } }");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("react"));
}

TEST(AuditFrameworkDetect, Inv4Qt6FromCMakeLists) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "CMakeLists.txt",
              "cmake_minimum_required(VERSION 3.20)\nfind_package(Qt6 REQUIRED)\n");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("qt6"));
}

TEST(AuditFrameworkDetect, DjangoFromManagePy) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "manage.py", "#!/usr/bin/env python\n");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("django"));
}

TEST(AuditFrameworkDetect, RustFromCargoToml) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "Cargo.toml",
              "[package]\nname = \"foo\"\nversion = \"0.1.0\"\n");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("rust"));
}

TEST(AuditFrameworkDetect, GoFromGoMod) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "go.mod", "module foo\n\ngo 1.21\n");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("go"));
}

TEST(AuditFrameworkDetect, EmptyProjectReturnsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.isEmpty());
}

TEST(AuditFrameworkDetect, MultipleFrameworksDetected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "requirements.txt", "flask\n");
    writeFile(tmp.path(), "package.json", "{ \"dependencies\": { \"react\": \"^18\" } }");
    const auto fws = AuditHygiene::detectProjectFrameworks(tmp.path());
    EXPECT_TRUE(fws.contains("flask"));
    EXPECT_TRUE(fws.contains("react"));
}

TEST(AuditFrameworkDetect, SemgrepRulePacksTwoArgsPerFramework) {
    const auto args = AuditHygiene::semgrepRulePacks(
        QStringList{"flask", "react"});
    ASSERT_EQ(args.size(), 4);
    EXPECT_EQ(args[0], "--config");
    EXPECT_EQ(args[1], "p/flask");
    EXPECT_EQ(args[2], "--config");
    EXPECT_EQ(args[3], "p/react");
}

TEST(AuditFrameworkDetect, SemgrepRulePacksUnknownContributesNothing) {
    const auto args = AuditHygiene::semgrepRulePacks(
        QStringList{"unknown-framework", "flask"});
    ASSERT_EQ(args.size(), 2);
    EXPECT_EQ(args[0], "--config");
    EXPECT_EQ(args[1], "p/flask");
}

TEST(AuditFrameworkDetect, SemgrepRulePacksEmptyInputEmptyOutput) {
    EXPECT_TRUE(AuditHygiene::semgrepRulePacks({}).isEmpty());
}
