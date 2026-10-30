// ANTS-5515 — dependency_inventory. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "dependencyinventory.h"
#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "../../_support/srcgrep.h"

namespace {

class Project {
public:
    void add(const QString &rel, const QByteArray &bytes) {
        const QString path = tmp.filePath(rel);
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).path()));
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        tracked.append(rel);
    }
    QJsonObject run() const { return DependencyInventory::run(tmp.path(), tracked); }
    QTemporaryDir tmp;
    QStringList tracked;
};

QList<QJsonObject> ofKind(const QJsonObject &env, const char *kind) {
    QList<QJsonObject> out;
    for (const auto &v : env.value("rows").toArray())
        if (v.toObject().value("kind").toString() == QLatin1String(kind))
            out.append(v.toObject());
    return out;
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

std::string str(const QJsonObject &o, const char *key) {
    return o.value(QLatin1String(key)).toString().toStdString();
}

}  // namespace

TEST(McpDependencyInventory, Ants5515UsesFormsStayApart) {
    Project p;
    p.add(".github/workflows/ci.yml",
          "jobs:\n"
          "  a:\n"
          "    steps:\n"
          "      - uses: actions/checkout@abc123 # v4.2.0\n"
          "      - uses: ./.github/actions/local\n"
          "      - uses: docker://alpine:3.20\n"
          "      # - uses: old/gone@v1\n"
          "      - uses: 'o/r/.github/workflows/x.yml@v2'\n");
    const QJsonObject env = p.run();
    const auto actions = ofKind(env, "action");
    ASSERT_EQ(actions.size(), 2) << dump(env);
    EXPECT_EQ(str(actions[0], "repo"), "actions/checkout");
    EXPECT_EQ(str(actions[0], "ref"), "abc123");
    EXPECT_EQ(str(actions[0], "version_comment"), "v4.2.0");
    EXPECT_EQ(str(actions[0], "file"), ".github/workflows/ci.yml");
    EXPECT_EQ(actions[0].value("line").toInt(), 4);
    EXPECT_EQ(str(actions[1], "repo"), "o/r");
    EXPECT_EQ(str(actions[1], "subpath"), ".github/workflows/x.yml");
    const auto local = ofKind(env, "local_action");
    ASSERT_EQ(local.size(), 1) << dump(env);
    EXPECT_EQ(str(local[0], "path"), "./.github/actions/local");
    const auto docker = ofKind(env, "docker_action");
    ASSERT_EQ(docker.size(), 1) << dump(env);
    EXPECT_EQ(str(docker[0], "image"), "alpine:3.20");
}

TEST(McpDependencyInventory, Ants5515RunnerForms) {
    Project p;
    p.add(".github/workflows/a.yaml",
          "  x:\n    runs-on: ubuntu-24.04 # pinned\n"
          "  y:\n    runs-on: [self-hosted, 'linux']\n"
          "  z:\n    runs-on:\n      - macos-14\n      - arm64\n    steps: []\n"
          "  w:\n    runs-on: ${{ matrix.os }}\n");
    const QJsonObject env = p.run();
    const auto rows = ofKind(env, "runner");
    QStringList labels;
    for (const auto &r : rows) labels.append(r.value("label").toString());
    EXPECT_EQ(labels, (QStringList{"ubuntu-24.04", "self-hosted", "linux", "macos-14", "arm64",
                                   "${{ matrix.os }}"})) << dump(env);
    EXPECT_EQ(rows.at(3).value("line").toInt(), 7);
    EXPECT_TRUE(rows.last().value("unresolved").toBool());
}

TEST(McpDependencyInventory, Ants5515ContainerImages) {
    Project p;
    p.add(".github/workflows/a.yml",
          "    container: ubuntu:24.04\n"
          "    services:\n      db:\n        image: registry.local:5000/db\n"
          "    container:\n      image: ${{ matrix.img }}\n");
    p.add("Dockerfile",
          "FROM --platform=linux/amd64 golang:1.23@sha256:ab AS build\n"
          "FROM build\nFROM scratch\nfrom alpine\n");
    p.add("tools/ci.Containerfile", "FROM $BASE\n");
    const QJsonObject env = p.run();
    const auto rows = ofKind(env, "container");
    ASSERT_EQ(rows.size(), 6) << dump(env);
    EXPECT_EQ(str(rows[0], "tag"), "24.04");
    EXPECT_EQ(str(rows[1], "image"), "registry.local:5000/db");
    EXPECT_FALSE(rows[1].contains("tag")) << "a port is not a tag";
    EXPECT_TRUE(rows[2].value("unresolved").toBool());
    EXPECT_EQ(str(rows[3], "tag"), "1.23");
    EXPECT_EQ(str(rows[3], "digest"), "sha256:ab");
    EXPECT_EQ(str(rows[3], "file"), "Dockerfile");
    EXPECT_EQ(str(rows[4], "image"), "alpine");
    EXPECT_EQ(rows[4].value("line").toInt(), 4);
    EXPECT_TRUE(rows[5].value("unresolved").toBool());
}

TEST(McpDependencyInventory, Ants5515RuntimeVersionsNameTheirFile) {
    Project p;
    p.add(".nvmrc", "\n20.11.0\n");
    p.add(".tool-versions", "ruby 3.3.0 # note\n");
    p.add("go.mod", "module x\n\ngo 1.23.1\n");
    p.add("pyproject.toml", "[project]\nrequires-python = \">=3.11\"\n");
    p.add("Cargo.toml", "[package]\nrust-version = \"1.80\"\n");
    p.add("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nfind_package(Qt6 6.2 REQUIRED)\n");
    p.add(".github/workflows/a.yml", "        with:\n          java-version: '21'\n");
    const QJsonObject env = p.run();
    QMap<std::string, std::string> found;
    for (const auto &r : ofKind(env, "runtime"))
        found[str(r, "name")] = str(r, "version") + " " + str(r, "file") + ":" +
                                std::to_string(r.value("line").toInt());
    EXPECT_EQ(found["node"], "20.11.0 .nvmrc:2") << dump(env);
    EXPECT_EQ(found["ruby"], "3.3.0 .tool-versions:1");
    EXPECT_EQ(found["go"], "1.23.1 go.mod:3");
    EXPECT_EQ(found["python"], ">=3.11 pyproject.toml:2");
    EXPECT_EQ(found["rust"], "1.80 Cargo.toml:2");
    EXPECT_EQ(found["cmake"], "3.21 CMakeLists.txt:1");
    EXPECT_EQ(found["qt"], "6.2 CMakeLists.txt:2");
    EXPECT_EQ(found["java"], "21 .github/workflows/a.yml:2");

    Project pkg;
    pkg.add("package.json", "{\n  \"engines\": {\n    \"node\": \">=20\"\n  }\n}\n");
    const auto rows = ofKind(pkg.run(), "runtime");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(str(rows[0], "version"), ">=20");
    EXPECT_EQ(rows[0].value("line").toInt(), 3);
}

TEST(McpDependencyInventory, Ants5515NoSourceIsSaid) {
    Project p;
    p.add("package.json", "{}\n");
    p.add("README.md", "Needs node 18 and go 1.20.\n");
    p.add("nested/go.mod", "go 1.20\n");
    const QJsonObject env = p.run();
    const auto rows = ofKind(env, "runtime");
    ASSERT_EQ(rows.size(), 1) << dump(env);
    EXPECT_EQ(str(rows[0], "name"), "node");
    EXPECT_TRUE(rows[0].value("version").isNull());
    EXPECT_TRUE(rows[0].value("no_authoritative_source").toBool());
    EXPECT_EQ(str(rows[0], "file"), "package.json");
}

TEST(McpDependencyInventory, Ants5515SilenceIsNamed) {
    QTemporaryDir tmp;
    const QJsonObject none = DependencyInventory::run(tmp.path(), {});
    EXPECT_TRUE(none.value("ok").toBool());
    EXPECT_EQ(none.value("skipped").toArray().at(0).toString(), QStringLiteral("tracked_files"));
    EXPECT_FALSE(none.value("not_covered").toArray().isEmpty());

    Project p;
    p.add("third_party/x/Dockerfile", "FROM a\n");
    p.add("third_party/x/CMakeLists.txt", "cmake_minimum_required(VERSION 2.8)\n");
    const QJsonObject env = p.run();
    EXPECT_TRUE(env.value("rows").toArray().isEmpty()) << dump(env);
    EXPECT_FALSE(env.value("not_covered").toArray().isEmpty());
}

TEST(McpDependencyInventory, Ants5515VerbIsWiredAndOffline) {
    const std::string reg = ants_test::slurpFile(ANTS_MCP_REGISTRY_SOURCE);
    EXPECT_NE(reg.find("registerToolProvider(\"dependency_inventory\""), std::string::npos);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_NE(ci.find("t[\"name\"] = \"dependency_inventory\""), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"dependency_inventory\")"), std::string::npos);

    // Nothing leaves the machine: lookups belong to the check-dependencies skill.
    const QString srcDir = QFileInfo(QString::fromUtf8(SRC_CLAUDE_INTEGRATION_CPP_PATH)).path();
    const std::string seam =
        ants_test::slurpFile((srcDir + QStringLiteral("/dependencyinventory.cpp")).toUtf8().constData());
    ASSERT_FALSE(seam.empty());
    for (const char *reach : {"QNetwork", "QProcess", "QTcpSocket", "QSslSocket", "curl"})
        EXPECT_EQ(seam.find(reach), std::string::npos) << reach;

    Project p;
    p.add("Dockerfile", "FROM a:1\n");
    RemoteControl rc(nullptr);
    const QJsonObject env =
        rc.cmdDependencyInventory(QJsonObject{{"caller_cwd", p.tmp.path()}}).object();
    EXPECT_TRUE(env.value("ok").toBool()) << dump(env);
    EXPECT_EQ(env.value("skipped").toArray().at(0).toString(), QStringLiteral("tracked_files"))
        << "the fixture is no git repository, and the reply must say what that cost";
}
