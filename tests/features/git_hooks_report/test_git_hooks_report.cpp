// Feature-conformance test for ANTS-5505, git_state op:"hooks".
// Contract: tests/features/git_hooks_report/spec.md

#include <gtest/gtest.h>

#include "githooksreport.h"
#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace {

bool git(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStandardPaths::findExecutable(QStringLiteral("git")), args);
    return p.waitForFinished(30000) && p.exitCode() == 0;
}

QString gitOut(const QString &cwd, const QStringList &args) {
    QProcess p;
    p.setWorkingDirectory(cwd);
    p.start(QStandardPaths::findExecutable(QStringLiteral("git")), args);
    p.waitForFinished(30000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

void write(const QString &path, const QByteArray &body, bool executable = false) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    f.write(body);
    f.close();
    if (executable)
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
}

QString initRepo(const QTemporaryDir &tmp) {
    const QString root = QDir(tmp.path()).filePath(QStringLiteral("repo"));
    QDir().mkpath(root);
    EXPECT_TRUE(git(root, {QStringLiteral("init"), QStringLiteral("-q")}));
    git(root, {QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@example.invalid")});
    git(root, {QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("t")});
    return QFileInfo(root).canonicalFilePath();
}

void commitAll(const QString &root) {
    ASSERT_TRUE(git(root, {QStringLiteral("add"), QStringLiteral("-A")}));
    ASSERT_TRUE(git(root, {QStringLiteral("commit"), QStringLiteral("-q"),
                           QStringLiteral("--no-verify"), QStringLiteral("-m"), QStringLiteral("x")}));
}

// A machine-wide hook that answers --gate-candidates with a short list.
QString machineHooks(const QTemporaryDir &tmp) {
    const QString dir = QDir(tmp.path()).filePath(QStringLiteral("machine-hooks"));
    write(dir + QStringLiteral("/pre-push"),
          "#!/usr/bin/env bash\n"
          "if [[ ${1:-} == --gate-candidates ]]; then\n"
          "  printf '%s\\n' scripts/local-ci.sh tools/local-ci.sh\n"
          "  exit 0\n"
          "fi\n"
          "exit 1\n",
          true);
    return dir;
}

QJsonObject hooksOp(const QString &root) {
    RemoteControl rc(nullptr);
    return rc.cmdGitState({{QStringLiteral("op"), QStringLiteral("hooks")},
                           {QStringLiteral("caller_cwd"), root}}).object();
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

}  // namespace

// This machine sets a global core.hooksPath; a fixture must not inherit it.
class GitStateHooks : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
    }
};

// The verb's own path: dispatch reaches the op, and the default machine-wide
// hook is whatever ANTS_GLOBAL_HOOKS names.
TEST_F(GitStateHooks, DispatchReachesTheOp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = initRepo(tmp);
    qputenv("ANTS_GLOBAL_HOOKS", machineHooks(tmp).toUtf8());
    const QJsonObject env = hooksOp(root);
    qunsetenv("ANTS_GLOBAL_HOOKS");
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);
    EXPECT_EQ(env.value(QStringLiteral("op")).toString(), QStringLiteral("hooks"));
    EXPECT_EQ(env.value(QStringLiteral("gate")).toObject()
                  .value(QStringLiteral("candidates_source")).toString(),
              QStringLiteral("machine_hook"));
}

// INV-1, INV-2 none, INV-4 discovered, INV-5, INV-6 unborn HEAD.
TEST_F(GitStateHooks, UnsetHooksPathAndDiscoveredGate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = initRepo(tmp);
    write(root + QStringLiteral("/tools/local-ci.sh"), "#!/bin/sh\n", true);
    write(root + QStringLiteral("/.github/workflows/ci.yml"), "run: tools/local-ci.sh\n");
    write(root + QStringLiteral("/.github/workflows/docs.yaml"), "run: echo docs\n");

    const QJsonObject env = GitHooksReport::run(root, machineHooks(tmp));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);

    const QJsonObject hp = env.value(QStringLiteral("hooks_path")).toObject();
    EXPECT_TRUE(hp.value(QStringLiteral("value")).isNull()) << dump(env);
    EXPECT_TRUE(hp.value(QStringLiteral("scope")).isNull());
    EXPECT_EQ(hp.value(QStringLiteral("dir")).toString(), root + QStringLiteral("/.git/hooks"));
    EXPECT_EQ(env.value(QStringLiteral("pre_push")).toObject().value(QStringLiteral("kind")).toString(),
              QStringLiteral("none"));

    const QJsonObject gate = env.value(QStringLiteral("gate")).toObject();
    EXPECT_EQ(gate.value(QStringLiteral("source")).toString(), QStringLiteral("discovered"));
    EXPECT_EQ(gate.value(QStringLiteral("path")).toString(), QStringLiteral("tools/local-ci.sh"));
    EXPECT_TRUE(gate.value(QStringLiteral("executable")).toBool());
    EXPECT_TRUE(gate.value(QStringLiteral("called_by_ci")).toBool());
    EXPECT_EQ(gate.value(QStringLiteral("workflows_calling")).toArray(),
              QJsonArray({QStringLiteral("ci.yml")}));
    EXPECT_EQ(env.value(QStringLiteral("workflows")).toArray(),
              QJsonArray({QStringLiteral("ci.yml"), QStringLiteral("docs.yaml")}));

    const QJsonObject passed = env.value(QStringLiteral("gate_passed")).toObject();
    EXPECT_TRUE(passed.value(QStringLiteral("tree")).isNull());
    EXPECT_FALSE(passed.value(QStringLiteral("passed")).toBool());
}

// INV-1 set locally, INV-2 hands_off / own / machine_wide, INV-6 a record.
TEST_F(GitStateHooks, HandOffKindsAndPassedRecord) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = initRepo(tmp);
    const QString machine = machineHooks(tmp);
    ASSERT_TRUE(git(root, {QStringLiteral("config"), QStringLiteral("core.hooksPath"),
                           QStringLiteral("tools/hooks")}));
    const QString hook = root + QStringLiteral("/tools/hooks/pre-push");
    write(hook,
          "#!/usr/bin/env bash\n"
          "global_hook=\"${ANTS_GLOBAL_HOOKS:-$HOME/.claude/githooks}/pre-push\"\n"
          "exec \"$global_hook\" \"$@\"\n",
          true);
    commitAll(root);

    QJsonObject env = GitHooksReport::run(root, machine);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);
    const QJsonObject hp = env.value(QStringLiteral("hooks_path")).toObject();
    EXPECT_EQ(hp.value(QStringLiteral("value")).toString(), QStringLiteral("tools/hooks"));
    EXPECT_EQ(hp.value(QStringLiteral("scope")).toString(), QStringLiteral("local"));
    EXPECT_EQ(hp.value(QStringLiteral("dir")).toString(), root + QStringLiteral("/tools/hooks"));
    EXPECT_EQ(env.value(QStringLiteral("pre_push")).toObject().value(QStringLiteral("kind")).toString(),
              QStringLiteral("hands_off"));

    // Names the shared hook, but only pipes to its secret scan: still its own.
    write(hook,
          "#!/usr/bin/env bash\n"
          "~/.claude/githooks/pre-push --secrets-only \"$@\"\n"
          "exec ./tools/local-ci.sh\n",
          true);
    env = GitHooksReport::run(root, machine);
    EXPECT_EQ(env.value(QStringLiteral("pre_push")).toObject().value(QStringLiteral("kind")).toString(),
              QStringLiteral("own")) << dump(env);

    ASSERT_TRUE(git(root, {QStringLiteral("config"), QStringLiteral("core.hooksPath"), machine}));
    env = GitHooksReport::run(root, machine);
    EXPECT_EQ(env.value(QStringLiteral("hooks_path")).toObject().value(QStringLiteral("dir")).toString(),
              machine);
    EXPECT_EQ(env.value(QStringLiteral("pre_push")).toObject().value(QStringLiteral("kind")).toString(),
              QStringLiteral("machine_wide")) << dump(env);

    const QString tree = gitOut(root, {QStringLiteral("rev-parse"), QStringLiteral("HEAD^{tree}")});
    write(root + QStringLiteral("/.git/ants-gate-passed/") + tree,
          "pre-push, 2026-10-21T23:18:24+02:00\n");
    const QJsonObject passed = GitHooksReport::run(root, machine)
                                   .value(QStringLiteral("gate_passed")).toObject();
    EXPECT_EQ(passed.value(QStringLiteral("tree")).toString(), tree);
    EXPECT_TRUE(passed.value(QStringLiteral("passed")).toBool());
    EXPECT_EQ(passed.value(QStringLiteral("record")).toString(),
              QStringLiteral("pre-push, 2026-10-21T23:18:24+02:00"));
}

// INV-3 and INV-4 configured: git config overrides .ants/gate.conf, and an
// absolute command inside the repository is made relative.
TEST_F(GitStateHooks, GateKeysAndConfiguredCommand) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = initRepo(tmp);
    write(root + QStringLiteral("/.ants/gate.conf"),
          "[ants \"gate\"]\n\tcommand = ./scripts/gate.sh\n\tdocsGlob = docs/*|*.md\n");
    write(root + QStringLiteral("/scripts/gate.sh"), "#!/bin/sh\n", true);

    QJsonObject env = GitHooksReport::run(root, machineHooks(tmp));
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);
    QJsonObject keys = env.value(QStringLiteral("gate_keys")).toObject();
    EXPECT_EQ(keys.value(QStringLiteral("ants.gate.docsglob")).toObject()
                  .value(QStringLiteral("value")).toString(), QStringLiteral("docs/*|*.md")) << dump(env);
    EXPECT_EQ(keys.value(QStringLiteral("ants.gate.command")).toObject()
                  .value(QStringLiteral("source")).toString(), QStringLiteral(".ants/gate.conf"));
    QJsonObject gate = env.value(QStringLiteral("gate")).toObject();
    EXPECT_EQ(gate.value(QStringLiteral("source")).toString(), QStringLiteral("config"));
    EXPECT_EQ(gate.value(QStringLiteral("path")).toString(), QStringLiteral("scripts/gate.sh"));
    EXPECT_FALSE(gate.contains(QStringLiteral("candidates_source")));
    EXPECT_FALSE(gate.value(QStringLiteral("called_by_ci")).toBool());

    ASSERT_TRUE(git(root, {QStringLiteral("config"), QStringLiteral("ants.gate.command"),
                           root + QStringLiteral("/scripts/gate.sh")}));
    env = GitHooksReport::run(root, machineHooks(tmp));
    keys = env.value(QStringLiteral("gate_keys")).toObject();
    EXPECT_EQ(keys.value(QStringLiteral("ants.gate.command")).toObject()
                  .value(QStringLiteral("source")).toString(), QStringLiteral("git_config"));
    gate = env.value(QStringLiteral("gate")).toObject();
    EXPECT_EQ(gate.value(QStringLiteral("path")).toString(), QStringLiteral("scripts/gate.sh")) << dump(env);
    EXPECT_FALSE(gate.contains(QStringLiteral("outside_repo")));

    ASSERT_TRUE(git(root, {QStringLiteral("config"), QStringLiteral("ants.gate.command"),
                           QStringLiteral("/nowhere/gate.sh")}));
    gate = GitHooksReport::run(root, machineHooks(tmp)).value(QStringLiteral("gate")).toObject();
    EXPECT_TRUE(gate.value(QStringLiteral("outside_repo")).toBool());
    EXPECT_EQ(gate.value(QStringLiteral("path")).toString(), QStringLiteral("/nowhere/gate.sh"));
}

// INV-4: no command, no candidate on the list, and a hook that cannot answer.
TEST_F(GitStateHooks, NoGateAndUnavailableCandidates) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = initRepo(tmp);
    const QJsonObject gate = GitHooksReport::run(root, tmp.path() + QStringLiteral("/absent"))
                                 .value(QStringLiteral("gate")).toObject();
    EXPECT_EQ(gate.value(QStringLiteral("candidates_source")).toString(), QStringLiteral("unavailable"));
    EXPECT_TRUE(gate.value(QStringLiteral("path")).isNull());
    EXPECT_FALSE(gate.contains(QStringLiteral("called_by_ci")));
}

// INV-7.
TEST_F(GitStateHooks, NotARepositoryRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString plain = QDir(tmp.path()).filePath(QStringLiteral("plain"));
    QDir().mkpath(plain);
    const QJsonObject env = GitHooksReport::run(QFileInfo(plain).canonicalFilePath(),
                                                machineHooks(tmp));
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(), QStringLiteral("not_git_repo")) << dump(env);
}
