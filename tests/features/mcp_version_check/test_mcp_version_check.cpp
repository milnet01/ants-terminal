// ANTS-5510 — version_check. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "remotecontrol.h"
#include "versioncheck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "../../_support/srcgrep.h"

namespace {

void write(const QTemporaryDir &tmp, const QString &rel, const QByteArray &bytes) {
    const QString path = tmp.filePath(rel);
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).path()));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(bytes);
}

// The source is `version.txt`; each extra entry is {path, pattern}.
QByteArray recipe(const QList<QPair<QString, QString>> &entries, const QJsonObject &extra = {}) {
    QJsonArray files;
    for (const auto &e : entries)
        files.append(QJsonObject{{"path", e.first}, {"pattern", e.second}, {"replace", "x"}});
    QJsonObject o = extra;
    o["version_source"] = "version.txt";
    o["version_pattern"] = "VERSION ([0-9.]+)";
    o["files"] = files;
    return QJsonDocument(o).toJson();
}

QList<QJsonObject> ofKind(const QJsonObject &env, const char *kind) {
    QList<QJsonObject> out;
    for (const auto &v : env.value("findings").toArray())
        if (v.toObject().value("kind").toString() == QLatin1String(kind))
            out.append(v.toObject());
    return out;
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

}  // namespace

TEST(McpVersionCheck, Ants5510AgreeingFilesAreClean) {
    QTemporaryDir tmp;
    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, "pkg/a.spec", "Version: 1.2.3\n");
    write(tmp, ".claude/bump.json", recipe({{"pkg/a.spec", "Version: {OLD}"}}));
    const QJsonObject env = VersionCheck::run(tmp.path(), {"pkg/a.spec"});
    ASSERT_TRUE(env.value("ok").toBool()) << dump(env);
    EXPECT_EQ(env.value("version").toString(), QStringLiteral("1.2.3"));
    EXPECT_TRUE(env.value("findings").toArray().isEmpty()) << dump(env);
    EXPECT_EQ(env.value("files").toArray().at(0).toObject().value("status").toString(),
              QStringLiteral("ok"));
}

TEST(McpVersionCheck, Ants5510DriftAndStraySuffix) {
    QTemporaryDir tmp;
    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, "a.txt", "first\nVersion: 1.2.2\n");
    write(tmp, "b.txt", "Version: 1.2.3-rc.1\n");
    write(tmp, ".claude/bump.json",
          recipe({{"a.txt", "Version: {OLD}"}, {"b.txt", "Version: {OLD}"}}));
    const QJsonObject env = VersionCheck::run(tmp.path(), {"a.txt"});
    const auto drift = ofKind(env, "version_drift");
    ASSERT_EQ(drift.size(), 1) << dump(env);
    EXPECT_EQ(drift.at(0).value("file").toString(), QStringLiteral("a.txt"));
    EXPECT_EQ(drift.at(0).value("line").toInt(), 2);
    EXPECT_EQ(drift.at(0).value("found").toString(), QStringLiteral("1.2.2"));
    EXPECT_EQ(drift.at(0).value("expected").toString(), QStringLiteral("1.2.3"));
    const auto stray = ofKind(env, "stray_suffix");
    ASSERT_EQ(stray.size(), 1) << dump(env);
    EXPECT_EQ(stray.at(0).value("found").toString(), QStringLiteral("1.2.3-rc.1"));
}

TEST(McpVersionCheck, Ants5510PatternOutcomes) {
    QTemporaryDir tmp;
    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, "none.txt", "nothing here\n");
    write(tmp, "two.txt", "v=1.2.3\nv=1.2.3\n");
    // Literal: the parentheses and the dot are text, and a suffix-looking tail in
    // the pattern's own context is not a suffix.
    write(tmp, "literal.txt", "pkg (1.2.3-1) unstable\n");
    write(tmp, ".claude/bump.json",
          recipe({{"none.txt", "v={OLD}"},
                  {"two.txt", "v={OLD}"},
                  {"gone.txt", "v={OLD}"},
                  {"literal.txt", "pkg ({OLD}-1) unstable"}}));
    const QJsonObject env = VersionCheck::run(tmp.path(), {"a"});
    EXPECT_EQ(ofKind(env, "pattern_not_found").size(), 1) << dump(env);
    EXPECT_EQ(ofKind(env, "pattern_ambiguous").size(), 1) << dump(env);
    EXPECT_EQ(ofKind(env, "file_missing").size(), 1) << dump(env);
    EXPECT_EQ(ofKind(env, "stray_suffix").size(), 0) << dump(env);
    EXPECT_EQ(ofKind(env, "version_drift").size(), 0) << dump(env);
}

TEST(McpVersionCheck, Ants5510UnlistedFiles) {
    QTemporaryDir tmp;
    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, "pkg/_service", "<param>v1.2.3</param>\n");
    write(tmp, "pkg/named-in-note.xml", "1.2.3\n");
    write(tmp, "notes.md", "1.2.3\n");
    write(tmp, "docs/a.txt", "1.2.3\n");
    write(tmp, "tests/a.txt", "1.2.3\n");
    write(tmp, "src/a.cpp", "// e.g. 1.2.3\n");
    write(tmp, "src/longer.txt", "11.2.3 and 1.2.34 and 1.2.3.4\n");
    write(tmp, ".claude/bump.json",
          recipe({}, QJsonObject{{"$note", "pkg/named-in-note.xml is hand-written"}}));
    const QJsonObject env = VersionCheck::run(
        tmp.path(), {"pkg/_service", "pkg/named-in-note.xml", "notes.md", "docs/a.txt",
                     "tests/a.txt", "src/a.cpp", "src/longer.txt"});
    const auto hits = ofKind(env, "unlisted_version_file");
    ASSERT_EQ(hits.size(), 1) << dump(env);
    EXPECT_EQ(hits.at(0).value("file").toString(), QStringLiteral("pkg/_service"));
    EXPECT_TRUE(hits.at(0).value("candidate").toBool());
    EXPECT_EQ(hits.at(0).value("line").toInt(), 1);
}

TEST(McpVersionCheck, Ants5510SilenceIsNamed) {
    QTemporaryDir tmp;
    const QJsonObject none = VersionCheck::run(tmp.path(), {"a"});
    EXPECT_TRUE(none.value("ok").toBool());
    EXPECT_FALSE(none.value("recipe_present").toBool(true));
    EXPECT_EQ(none.value("skipped").toArray().size(), 2);

    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, ".claude/bump.json", recipe({}));
    const QJsonObject noList = VersionCheck::run(tmp.path(), {});
    EXPECT_TRUE(noList.value("recipe_present").toBool());
    EXPECT_EQ(noList.value("skipped").toArray().at(0).toString(), QStringLiteral("unlisted"));
}

TEST(McpVersionCheck, Ants5510Refusals) {
    QTemporaryDir tmp;
    const auto code = [&tmp](const QByteArray &bump) {
        write(tmp, ".claude/bump.json", bump);
        const QJsonObject env = VersionCheck::run(tmp.path(), {"a"});
        EXPECT_FALSE(env.value("ok").toBool()) << dump(env);
        return env.value("code").toString().toStdString();
    };
    write(tmp, "version.txt", "no version\n");
    EXPECT_EQ(code("not json"), "bad_recipe");
    EXPECT_EQ(code("{}"), "bad_recipe");
    EXPECT_EQ(code(R"j({"version_source":"version.txt","version_pattern":"("})j"), "bad_recipe");
    EXPECT_EQ(code(R"j({"version_source":"version.txt","version_pattern":"x","post_bump":{}})j"),
              "bad_recipe");
    for (const char *entry : {R"j({"path":"a","pattern":"v={OLD}","format":"x"})j",
                              R"j({"path":"a","pattern":"(?P<v>x)"})j",
                              R"j({"path":"a","pattern":"^v"})j",
                              R"j({"path":"a","pattern":"v$"})j"}) {
        EXPECT_EQ(code(QByteArray(R"j({"version_source":"version.txt","version_pattern":"V (x)","files":[)j") +
                       entry + "]}"),
                  "bad_recipe") << entry;
    }
    EXPECT_EQ(code(recipe({})), "version_unreadable");
    EXPECT_EQ(code(R"j({"version_source":"gone.txt","version_pattern":"V (x)"})j"),
              "version_unreadable");
}

TEST(McpVersionCheck, Ants5510VerbIsWired) {
    const std::string reg = ants_test::slurpFile(ANTS_MCP_REGISTRY_SOURCE);
    EXPECT_NE(reg.find("registerToolProvider(\"version_check\""), std::string::npos);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_NE(ci.find("t[\"name\"] = \"version_check\""), std::string::npos);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"version_check\")"), std::string::npos);

    // The handler reaches the engine with git's file list.
    QTemporaryDir tmp;
    write(tmp, "version.txt", "VERSION 1.2.3\n");
    write(tmp, ".claude/bump.json", recipe({}));
    RemoteControl rc(nullptr);
    const QJsonObject env =
        rc.cmdVersionCheck(QJsonObject{{"caller_cwd", tmp.path()}}).object();
    EXPECT_TRUE(env.value("ok").toBool()) << dump(env);
    EXPECT_EQ(env.value("version").toString(), QStringLiteral("1.2.3"));
}
