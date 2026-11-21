// ANTS-6855 to ANTS-6859 — the audit's Claude hand-off starts on real
// findings. Contract: spec.md beside this file.

#include "auditdialog.h"
#include "auditengine.h"
#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/xdg_guard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

const AuditCheck *findCheck(const QList<AuditCheck> &checks, const QString &id) {
    for (const AuditCheck &c : checks)
        if (c.id == id) return &c;
    return nullptr;
}

// A check's own command, run as AuditDialog::runNextCheck runs it, then its
// filter: the lines that become findings.
QStringList runCheck(const AuditCheck &c, const QString &project) {
    QProcess p;
    p.setWorkingDirectory(project);
    p.start(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), c.command});
    p.waitForFinished(30000);
    const QString raw = QString::fromUtf8(p.readAllStandardOutput());
    const QString body = AuditEngine::applyFilter(raw, c.filter, project).body;
    return body.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

bool anyContains(const QStringList &lines, const QString &needle) {
    for (const QString &l : lines)
        if (l.contains(needle)) return true;
    return false;
}

void git(const QString &dir, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), args);
    p.waitForFinished(10000);
}

constexpr char kPublicPem[] =
    "-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEAplaceholderplaceholder=\n-----END PUBLIC KEY-----\n";
constexpr char kPrivatePem[] =
    "-----BEGIN PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIplaceholder=\n-----END PRIVATE KEY-----\n";

// A scratch project: C++ sources, a git index, two key files and the
// project's own CLAUDE.md. Its sample lines avoid the words the rules drop as
// noise ("test", "placeholder", "const char").
struct Project {
    QTemporaryDir dir;
    QString path() const { return dir.path(); }
    bool make() {
        if (!dir.isValid()) return false;
        bool ok = writeFile(dir.filePath("CMakeLists.txt"), "project(x)\n")
            && writeFile(dir.filePath("CLAUDE.md"), "# x\n\nSENTINEL-RULE-TEXT stays in the file.\n")
            && writeFile(dir.filePath("keys/update.pub.pem"), kPublicPem)
            && writeFile(dir.filePath("keys/server.pem"), kPrivatePem)
            && writeFile(dir.filePath("src/secretredact.h"), "#pragma once\n")
            && writeFile(dir.filePath("src/settings.cpp"),
                         "void f() {\n"
                         "    req.apiKey   = m_config->aiApiKey();\n"
                         "    api_key = \"AIzaSyD1x2y3z4w5v6u7t8s9q0\";\n"
                         "    auto n = QRandomGenerator::system()->generate();\n"
                         "    (void)!system(cmd);\n"
                         "}\n")
            && writeFile(dir.filePath("deploy.yaml"),
                         // Low entropy, so the push gate's gitleaks passes it.
                         "api_key: zzzzzzzzzzzzzzzzzzzz\n");
        git(path(), {"init", "-q"});
        git(path(), {"add", "-A"});
        return ok;
    }
};

class AuditClaudeHandoff : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(project.make()); }
    ants_test::XdgGuard env;
    Project project;
    Config cfg;
};

}  // namespace

TEST_F(AuditClaudeHandoff, BaselineIsCheapAndStandardsAreNamed) {
    AuditDialog dlg(project.path(), nullptr, &cfg);
    CheckResult r;
    r.checkId = QStringLiteral("x");
    r.checkName = QStringLiteral("X");
    Finding f;
    f.checkId = r.checkId;
    f.file = QStringLiteral("src/settings.cpp");
    f.line = 2;
    f.message = QStringLiteral("a finding");
    r.findings = {f};
    r.findingCount = 1;
    const QString text = dlg.plainTextResultsForTest({r});
    ASSERT_FALSE(text.isEmpty());

    // INV-1
    EXPECT_FALSE(text.contains(QStringLiteral("Run the existing test suite")))
        << "INV-1: the baseline step runs the whole suite";
    EXPECT_TRUE(text.contains(QStringLiteral("Do not rebuild or")))
        << "INV-1: the baseline step does not say to skip the full run";
    // INV-2
    EXPECT_TRUE(text.contains(QStringLiteral("=== Project rules ===")));
    EXPECT_TRUE(text.contains(QStringLiteral("CLAUDE.md — Project conventions")))
        << "INV-2: an existing standards file is not named";
    EXPECT_FALSE(text.contains(QStringLiteral("SENTINEL-RULE-TEXT")))
        << "INV-2: a standards file was pasted in";
    EXPECT_FALSE(text.contains(QStringLiteral("docs/standards/coding.md")))
        << "INV-2: a missing standards file is named";
}

TEST_F(AuditClaudeHandoff, ClangTidyKeepsOnlyDiagnostics) {
    AuditDialog dlg(project.path(), nullptr, &cfg);
    const AuditCheck *c = findCheck(dlg.checksForTest(), QStringLiteral("clang_tidy"));
    ASSERT_NE(c, nullptr) << "clang_tidy lane missing for a C++ project";
    // Captured from clang-tidy 23 on this tree, 2026-11-21.
    const QString raw = QStringLiteral(
        "[1/1] Processing file /p/src/a.cpp.\n"
        "77 warnings generated.\n"
        "./src/mcptoolsink.h:43:12: warning: enum 'DispatchLane' uses a larger base type [performance-enum-size]\n"
        "   43 | enum class DispatchLane { Shared, Bulk };\n"
        "      |            ^\n"
        "/p/src/terminalgrid.h:875:55: note: make conversion explicit to silence this warning\n"
        "/p/src/b.cpp:7:3: error: unknown type name 'QElapsedTimer' [clang-diagnostic-error]\n"
        "Suppressed 77 warnings (75 in non-user code, 2 with check filters).\n"
        "Use -header-filter=.* to display errors from all non-system headers.\n"
        "clang-tidy: headers not resolved (regenerate compile_commands.json)\n");
    const QStringList kept = AuditEngine::applyFilter(raw, c->filter, project.path())
                                 .body.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    EXPECT_EQ(kept, (QStringList{
        QStringLiteral("./src/mcptoolsink.h:43:12: warning: enum 'DispatchLane' uses a larger base type [performance-enum-size]"),
        QStringLiteral("/p/src/b.cpp:7:3: error: unknown type name 'QElapsedTimer' [clang-diagnostic-error]"),
        QStringLiteral("clang-tidy: headers not resolved (regenerate compile_commands.json)")}))
        << "INV-3";
}

TEST_F(AuditClaudeHandoff, PublicKeysAreNotExposedKeyFiles) {
    AuditDialog dlg(project.path(), nullptr, &cfg);
    const QList<AuditCheck> checks = dlg.checksForTest();

    const AuditCheck *env = findCheck(checks, QStringLiteral("env_files"));
    ASSERT_NE(env, nullptr);
    const QStringList envHits = runCheck(*env, project.path());
    EXPECT_TRUE(anyContains(envHits, QStringLiteral("server.pem"))) << "INV-4: private key missed";
    EXPECT_FALSE(anyContains(envHits, QStringLiteral("update.pub.pem"))) << "INV-4: public key flagged";

    const AuditCheck *gitSens = findCheck(checks, QStringLiteral("git_sensitive"));
    ASSERT_NE(gitSens, nullptr) << "git_sensitive lane missing for a git project";
    const QStringList gitHits = runCheck(*gitSens, project.path());
    EXPECT_TRUE(anyContains(gitHits, QStringLiteral("server.pem"))) << "INV-5: private key missed";
    EXPECT_FALSE(anyContains(gitHits, QStringLiteral("update.pub.pem"))) << "INV-5: public key flagged";
    EXPECT_FALSE(anyContains(gitHits, QStringLiteral("secretredact.h"))) << "INV-5: source name flagged";
}

TEST_F(AuditClaudeHandoff, SecretAndCommandRulesSkipTheFalseAlarms) {
    AuditDialog dlg(project.path(), nullptr, &cfg);
    const QList<AuditCheck> checks = dlg.checksForTest();

    const AuditCheck *secrets = findCheck(checks, QStringLiteral("secrets_scan"));
    ASSERT_NE(secrets, nullptr);
    const QStringList secretHits = runCheck(*secrets, project.path());
    EXPECT_FALSE(anyContains(secretHits, QStringLiteral("aiApiKey()"))) << "INV-6: settings read flagged";
    EXPECT_TRUE(anyContains(secretHits, QStringLiteral("AIzaSyD1x2y3z4w5v6u7t8s9q0"))) << "INV-6: literal missed";
    EXPECT_TRUE(anyContains(secretHits, QStringLiteral("zzzzzzzzzzzzzzzzzzzz"))) << "INV-6: YAML value missed";

    const AuditCheck *cmd = findCheck(checks, QStringLiteral("cmd_injection"));
    ASSERT_NE(cmd, nullptr);
    const QStringList cmdHits = runCheck(*cmd, project.path());
    EXPECT_FALSE(anyContains(cmdHits, QStringLiteral("QRandomGenerator::system()"))) << "INV-7";
    EXPECT_TRUE(anyContains(cmdHits, QStringLiteral("system(cmd)"))) << "INV-7: real call missed";
}
