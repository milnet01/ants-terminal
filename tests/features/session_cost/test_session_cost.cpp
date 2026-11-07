// ANTS-5698 — session_cost. Contract: spec.md beside this file; the test for
// each invariant is named there.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "sessioncostverb.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>

namespace {

QByteArray line(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject assistant(const QString &id, int in, int read, int write, int out,
                      const QJsonArray &content = {}, bool sidechain = false) {
    return QJsonObject{
        {"type", "assistant"}, {"isSidechain", sidechain},
        {"message", QJsonObject{
            {"id", id}, {"content", content},
            {"usage", QJsonObject{{"input_tokens", in},
                                  {"cache_read_input_tokens", read},
                                  {"cache_creation_input_tokens", write},
                                  {"output_tokens", out}}}}}};
}

QJsonObject toolUse(const QString &id, const QString &name) {
    return QJsonObject{{"type", "tool_use"}, {"id", id}, {"name", name}};
}

QJsonObject toolResult(const QString &useId, const QJsonValue &content) {
    return QJsonObject{
        {"type", "user"},
        {"message", QJsonObject{{"role", "user"},
                                {"content", QJsonArray{QJsonObject{
                                    {"type", "tool_result"},
                                    {"tool_use_id", useId},
                                    {"content", content}}}}}}};
}

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

}  // namespace

TEST(SessionCost, Inv1RequestsCountedOncePerMessageId) {
    QTemporaryDir dir;
    const QString path = dir.filePath("s.jsonl");
    QByteArray t;
    t += line(assistant("m1", 10, 1000, 500, 20));
    t += line(assistant("m1", 10, 1000, 500, 20));  // second block, same message
    t += line(assistant("m2", 5, 1600, 100, 30));
    t += line(assistant("m3", 2, 1200, 0, 10));
    t += line(assistant("side", 1, 99999, 0, 1, {}, true));  // subagent line
    ASSERT_TRUE(writeFile(path, t));

    const QJsonObject r = SessionCost::analyse(path, 8);
    EXPECT_EQ(r.value("requests").toInt(), 3);
    const QJsonObject c = r.value("context").toObject();
    EXPECT_EQ(c.value("first").toInt(), 1510);
    EXPECT_EQ(c.value("current").toInt(), 1202);
    EXPECT_EQ(c.value("peak").toInt(), 1705);
    const QJsonObject s = r.value("totals").toObject();
    EXPECT_EQ(s.value("input").toInt(), 17);
    EXPECT_EQ(s.value("cache_read").toInt(), 3800);
    EXPECT_EQ(s.value("cache_write").toInt(), 600);
    EXPECT_EQ(s.value("output").toInt(), 60);
}

TEST(SessionCost, Inv2LargestItemsNameTheTool) {
    QTemporaryDir dir;
    const QString path = dir.filePath("s.jsonl");
    QByteArray t;
    t += line(assistant("m1", 1, 100, 0, 1,
                        QJsonArray{toolUse("u1", "Read"), toolUse("u2", "Bash")}));
    t += line(toolResult("u1", QString(5000, 'x')));
    t += line(toolResult("u2", QJsonArray{
        QJsonObject{{"type", "text"}, {"text", QString(300, 'y')}},
        QJsonObject{{"type", "image"},
                    {"source", QJsonObject{{"type", "base64"},
                                           {"data", QString(90000, 'A')}}}}}));
    t += line(QJsonObject{{"type", "user"}, {"isMeta", true},
                          {"message", QJsonObject{{"role", "user"},
                                                  {"content", QString(2000, 'z')}}}});
    t += line(assistant("m2", 1, 9000, 0, 1));
    ASSERT_TRUE(writeFile(path, t));

    const QJsonObject r = SessionCost::analyse(path, 8);
    EXPECT_EQ(r.value("images").toInt(), 1);
    const QJsonArray items = r.value("largest_items").toArray();
    ASSERT_EQ(items.size(), 3);
    const QJsonObject first = items.at(0).toObject();
    EXPECT_EQ(first.value("kind").toString(), "tool_result");
    EXPECT_EQ(first.value("tool").toString(), "Read");
    EXPECT_EQ(first.value("bytes").toInt(), 5000);
    EXPECT_EQ(first.value("request").toInt(), 2);
    EXPECT_EQ(items.at(1).toObject().value("kind").toString(), "meta_text");
    const QJsonObject bash = items.at(2).toObject();
    EXPECT_EQ(bash.value("tool").toString(), "Bash");
    EXPECT_EQ(bash.value("bytes").toInt(), 300) << "an image is never sized by its base64";

    const QJsonObject capped = SessionCost::analyse(path, 1);
    EXPECT_EQ(capped.value("largest_items").toArray().size(), 1);
}

TEST(SessionCost, Inv3StartupFromRequestShape) {
    QTemporaryDir dir;
    const QString path = dir.filePath("s.jsonl");
    const QJsonObject big{{"name", "Big"}, {"description", QString(900, 'd')}};
    const QJsonObject small{{"name", "Small"}, {"description", "s"}};
    QByteArray t;
    t += line(QJsonObject{
        {"type", "api-request-shape"},
        {"shape", QJsonObject{
            {"system", QJsonArray{QJsonObject{{"type", "text"}, {"text", QString(400, 's')}},
                                  QJsonObject{{"type", "text"}, {"text", QString(100, 's')}}}},
            {"tools", QJsonArray{small, big}}}}});
    t += line(assistant("m1", 1, 100, 0, 1));
    ASSERT_TRUE(writeFile(path, t));

    const QJsonObject s = SessionCost::analyse(path, 8).value("startup").toObject();
    EXPECT_EQ(s.value("system_bytes").toInt(), 500);
    EXPECT_EQ(s.value("tool_count").toInt(), 2);
    const QJsonArray tools = s.value("largest_tools").toArray();
    ASSERT_EQ(tools.size(), 2);
    EXPECT_EQ(tools.at(0).toObject().value("name").toString(), "Big");
    EXPECT_EQ(s.value("tool_bytes").toInt(),
              tools.at(0).toObject().value("bytes").toInt()
                  + tools.at(1).toObject().value("bytes").toInt());

    const QString bare = dir.filePath("bare.jsonl");
    ASSERT_TRUE(writeFile(bare, line(assistant("m1", 1, 100, 0, 1))));
    const QJsonObject r = SessionCost::analyse(bare, 8);
    EXPECT_TRUE(r.value("ok").toBool());
    EXPECT_TRUE(r.contains("startup"));
    EXPECT_TRUE(r.value("startup").isNull());
}

TEST(SessionCost, Inv4Refusals) {
    QTemporaryDir dir;
    const QStringList projects{dir.path()};
    const QString cwd = QStringLiteral("/mnt/Some_Project");
    ASSERT_TRUE(writeFile(dir.filePath(SessionCost::folderNameFor(cwd) + "/abcdef12.jsonl"),
                          line(assistant("m1", 1, 1, 1, 1))));
    EXPECT_EQ(SessionCost::folderNameFor(cwd), "-mnt-Some-Project");

    const auto code = [&](const QString &c, const QJsonObject &args) {
        return SessionCost::run(projects, c, args, 0).value("code").toString();
    };
    EXPECT_EQ(code(cwd, {{"session_id", "../../etc"}}), "bad_args");
    EXPECT_EQ(code(cwd, {{"session_id", "ffffffff"}}), "not_found");
    EXPECT_EQ(code(QStringLiteral("/nowhere/at/all"), {}), "not_found");
    EXPECT_TRUE(SessionCost::run(projects, cwd, {{"session_id", "abcdef12"}}, 0)
                    .value("ok").toBool());
    EXPECT_TRUE(SessionCost::run(projects, cwd + "/sub/dir", {}, 0)
                    .value("ok").toBool()) << "an ancestor's folder is found";
}

TEST(SessionCost, Inv5NewestWhenUnnamed) {
    QTemporaryDir dir;
    const QString cwd = QStringLiteral("/p");
    const QString folder = dir.filePath(SessionCost::folderNameFor(cwd));
    ASSERT_TRUE(writeFile(folder + "/olderaaa.jsonl", line(assistant("m1", 1, 1, 1, 1))));
    ASSERT_TRUE(writeFile(folder + "/newerbbb.jsonl", line(assistant("m1", 1, 1, 1, 1))));
    QFile older(folder + "/olderaaa.jsonl");
    ASSERT_TRUE(older.open(QIODevice::ReadWrite));
    const QDateTime now = QDateTime::currentDateTime();
    ASSERT_TRUE(older.setFileTime(now.addSecs(-3600), QFileDevice::FileModificationTime));
    older.close();

    const QJsonObject r =
        SessionCost::run({dir.path()}, cwd, {}, now.toMSecsSinceEpoch());
    EXPECT_EQ(r.value("session_id").toString(), "newerbbb");
    EXPECT_TRUE(r.value("session_inferred").toBool());
    EXPECT_EQ(r.value("other_active_sessions").toInt(), 0);
}

namespace {

// A session transcript and one helper agent's, with set mtimes.
struct HelperFixture {
    QTemporaryDir dir;
    QString cwd = QStringLiteral("/p");
    QString folder = dir.filePath(SessionCost::folderNameFor(cwd));
    QString session = folder + "/main1234.jsonl";
    QString helper = folder + "/main1234/subagents/agent-a7487c40babb.jsonl";
    QDateTime now = QDateTime::currentDateTime();

    bool write(qint64 sessionAgeSecs, qint64 helperAgeSecs) {
        QByteArray h;  // a helper's own lines all carry isSidechain
        h += line(assistant("h1", 1, 100, 0, 1, {}, true));
        h += line(assistant("h2", 1, 300, 0, 1, {}, true));
        return writeFile(session, line(assistant("m1", 1, 1, 1, 1)))
            && writeFile(helper, h)
            && age(session, sessionAgeSecs) && age(helper, helperAgeSecs);
    }
    bool age(const QString &path, qint64 secs) {
        QFile f(path);
        return f.open(QIODevice::ReadWrite)
            && f.setFileTime(now.addSecs(-secs), QFileDevice::FileModificationTime);
    }
    QJsonObject run(const QString &id) {
        const QJsonObject args = id.isEmpty() ? QJsonObject{}
                                              : QJsonObject{{"session_id", id}};
        return SessionCost::run({dir.path()}, cwd, args, now.toMSecsSinceEpoch());
    }
};

}  // namespace

// INV-7
TEST(SessionCost, Inv7HelperTranscriptCountsItsOwnLines) {
    HelperFixture f;
    ASSERT_TRUE(f.write(60, 60));
    const QJsonObject r = SessionCost::analyse(f.helper, 8);
    EXPECT_EQ(r.value("requests").toInt(), 2);
    EXPECT_EQ(r.value("context").toObject().value("peak").toInt(), 301);
}

// INV-8
TEST(SessionCost, Inv8AgentIdReadsTheHelper) {
    HelperFixture f;
    ASSERT_TRUE(f.write(60, 60));
    const QJsonObject r = f.run("agent-a7487c40babb");
    ASSERT_TRUE(r.value("ok").toBool()) << r.value("error").toString().toStdString();
    EXPECT_EQ(r.value("session_id").toString(), "agent-a7487c40babb");
    EXPECT_EQ(r.value("parent_session_id").toString(), "main1234");
    EXPECT_EQ(r.value("requests").toInt(), 2);
    EXPECT_FALSE(r.value("session_inferred").toBool());
    EXPECT_EQ(f.run("agent-ffffffffffff").value("code").toString(), "not_found");
}

// INV-9
TEST(SessionCost, Inv9SelfIsTheNewestSessionOrHelper) {
    HelperFixture f;
    ASSERT_TRUE(f.write(120, 5));
    QJsonObject r = f.run("self");
    EXPECT_EQ(r.value("session_id").toString(), "agent-a7487c40babb");
    EXPECT_TRUE(r.value("session_inferred").toBool());
    EXPECT_EQ(r.value("other_active_sessions").toInt(), 1);
    EXPECT_EQ(f.run(QString()).value("session_id").toString(), "main1234")
        << "with no id, helpers are never read";

    ASSERT_TRUE(f.age(f.session, 1));
    r = f.run("self");
    EXPECT_EQ(r.value("session_id").toString(), "main1234");
}

// INV-16
TEST(SessionCost, Inv16IncludeAgentsSumsHelpers) {
    HelperFixture f;
    ASSERT_TRUE(f.write(60, 60));
    const auto runWith = [&f](const QJsonObject &args) {
        return SessionCost::run({f.dir.path()}, f.cwd, args, f.now.toMSecsSinceEpoch());
    };
    const QJsonObject r = runWith({{"session_id", "main1234"}, {"include_agents", true}});
    ASSERT_TRUE(r.value("ok").toBool()) << r.value("error").toString().toStdString();
    const QJsonObject agents = r.value("agents").toObject();
    EXPECT_EQ(agents.value("count").toInt(), 1);
    EXPECT_EQ(agents.value("requests").toInt(), 2);
    EXPECT_EQ(agents.value("totals").toObject().value("cache_read").toInt(), 400);
    EXPECT_TRUE(agents.contains("rent_total"));
    EXPECT_FALSE(runWith({{"session_id", "main1234"}}).contains("agents"));
    EXPECT_FALSE(runWith({{"session_id", "agent-a7487c40babb"}, {"include_agents", true}})
                     .contains("agents")) << "a helper has no helpers of its own";
}

// ANTS-6697 — rent by tool, server and hook.
namespace {

QJsonObject toolDef(const QString &name, int descBytes) {
    return QJsonObject{{"name", name}, {"description", QString(descBytes, 'd')}};
}
qint64 defBytes(const QJsonObject &def) {
    return QJsonDocument(def).toJson(QJsonDocument::Compact).size();
}
QJsonObject shapeLine(const QJsonArray &tools) {
    return QJsonObject{{"type", "api-request-shape"},
                       {"shape", QJsonObject{{"system", QJsonArray{}}, {"tools", tools}}}};
}
QJsonObject attachment(const QJsonObject &a, int renderedBytes) {
    return QJsonObject{
        {"type", "attachment"}, {"attachment", a}, {"renderedRole", "system"},
        {"rendered", QJsonArray{QJsonObject{{"content", QString(renderedBytes, 'h')}}}}};
}
QJsonObject findRow(const QJsonArray &rows, const QString &key, const QString &value) {
    for (const auto &v : rows)
        if (v.toObject().value(key).toString() == value) return v.toObject();
    return {};
}

// Five requests; the fourth's context (400) is under half the third's
// (1300), so requests 1-3 and 4-5 are two segments.
struct RentFixture {
    QTemporaryDir dir;
    QString path = dir.filePath("s.jsonl");
    QJsonObject a = toolDef("mcp__ants__a", 100);
    QJsonObject read = toolDef("Read", 50);
    QJsonObject b = toolDef("mcp__ants__b", 70);

    bool write() {
        QByteArray t;
        t += line(shapeLine(QJsonArray{a, read}));
        t += line(attachment(QJsonObject{{"type", "hook_additional_context"},
                                         {"hookName", "SessionStart"}}, 200));
        t += line(attachment(QJsonObject{{"type", "deferred_tools_delta"},
                                         {"addedNames", QJsonArray{"mcp__ants__b", "Cron"}}},
                             40));
        t += line(assistant("m1", 0, 1000, 0, 1,
                            QJsonArray{toolUse("u1", "mcp__ants__a"), toolUse("u2", "Read")}));
        t += line(toolResult("u1", QString(300, 'x')));
        t += line(toolResult("u2", QString(100, 'x')));
        t += line(shapeLine(QJsonArray{a, read, b}));
        t += line(assistant("m2", 0, 1200, 0, 1, QJsonArray{toolUse("u3", "mcp__ants__a")}));
        t += line(toolResult("u3", QString(50, 'x')));
        t += line(assistant("m3", 0, 1300, 0, 1));
        t += line(assistant("m4", 0, 400, 0, 1));
        t += line(assistant("m5", 0, 500, 0, 1));
        return writeFile(path, t);
    }
};

}  // namespace

// INV-10
TEST(SessionCost, Inv10RentStopsAtACompaction) {
    RentFixture f;
    ASSERT_TRUE(f.write());
    const QJsonObject r = SessionCost::analyse(f.path, 8);
    EXPECT_EQ(r.value("compactions").toInt(), 1);
    // The hook text arrives before request 1 and is held for requests 1-3.
    EXPECT_EQ(r.value("hooks").toObject().value("rent_bytes").toInt(), 200 * 3);
    // u3 arrives before request 3, the segment's last.
    const QJsonObject a = findRow(r.value("tools").toArray(), "name", "mcp__ants__a");
    EXPECT_EQ(a.value("rent_bytes").toInt(), 300 * 2 + 50 * 1);
}

// INV-11 to INV-14
TEST(SessionCost, Inv11ToolsAndServers) {
    RentFixture f;
    ASSERT_TRUE(f.write());
    const QJsonObject r = SessionCost::analyse(f.path, 8);

    const QJsonArray tools = r.value("tools").toArray();
    ASSERT_EQ(tools.size(), 2);
    const QJsonObject a = tools.at(0).toObject();  // largest rent first
    EXPECT_EQ(a.value("name").toString(), "mcp__ants__a");
    EXPECT_EQ(a.value("server").toString(), "ants");
    EXPECT_EQ(a.value("calls").toInt(), 2);
    EXPECT_EQ(a.value("result_bytes").toInt(), 350);
    const QJsonObject rd = tools.at(1).toObject();
    EXPECT_EQ(rd.value("server").toString(), "builtin");
    EXPECT_EQ(rd.value("rent_bytes").toInt(), 100 * 2);
    EXPECT_EQ(SessionCost::analyse(f.path, 1).value("tools").toArray().size(), 1);

    const QJsonObject su = r.value("startup").toObject();
    const QJsonObject suAnts = findRow(su.value("servers").toArray(), "server", "ants");
    EXPECT_EQ(suAnts.value("tool_count").toInt(), 1);
    EXPECT_EQ(suAnts.value("bytes").toInt(), defBytes(f.a));
    const QJsonArray later = su.value("loaded_later").toArray();
    ASSERT_EQ(later.size(), 1);
    EXPECT_EQ(later.at(0).toObject().value("server").toString(), "ants");
    EXPECT_EQ(later.at(0).toObject().value("tool_count").toInt(), 1);
    EXPECT_EQ(later.at(0).toObject().value("bytes").toInt(), defBytes(f.b));

    const QJsonObject hooks = r.value("hooks").toObject();
    EXPECT_EQ(hooks.value("count").toInt(), 1);
    EXPECT_EQ(hooks.value("bytes").toInt(), 200);
    EXPECT_EQ(findRow(hooks.value("by_hook").toArray(), "hook", "SessionStart")
                  .value("rent_bytes").toInt(), 600);
    const QJsonObject dt = r.value("deferred_tools").toObject();
    EXPECT_EQ(dt.value("count").toInt(), 1);
    EXPECT_EQ(dt.value("bytes").toInt(), 40);
    EXPECT_EQ(dt.value("rent_bytes").toInt(), 40 * 3);

    // Request 1 carries the first shape; requests 2-5 the second.
    const qint64 antsDefs = defBytes(f.a) * 5 + defBytes(f.b) * 4;
    const qint64 builtinDefs = defBytes(f.read) * 5;
    const QJsonObject ants = findRow(r.value("servers").toArray(), "server", "ants");
    EXPECT_EQ(ants.value("calls").toInt(), 2);
    EXPECT_EQ(ants.value("result_bytes").toInt(), 350);
    EXPECT_EQ(ants.value("result_rent_bytes").toInt(), 650);
    EXPECT_EQ(ants.value("definition_rent_bytes").toInt(), antsDefs);
    EXPECT_EQ(ants.value("deferred_name_rent_bytes").toInt(), 13 * 3);  // "mcp__ants__b\n"
    EXPECT_EQ(ants.value("rent_bytes").toInt(), 650 + antsDefs + 39);
    const QJsonObject builtin = findRow(r.value("servers").toArray(), "server", "builtin");
    EXPECT_EQ(builtin.value("deferred_name_rent_bytes").toInt(), 5 * 3);  // "Cron\n"

    const QJsonObject rent = r.value("rent").toObject();
    EXPECT_EQ(rent.value("results").toInt(), 850);
    EXPECT_EQ(rent.value("definitions").toInt(), antsDefs + builtinDefs);
    EXPECT_EQ(rent.value("hooks").toInt(), 600);
    EXPECT_EQ(rent.value("deferred_tools").toInt(), 120);
    EXPECT_EQ(rent.value("total").toInt(), 850 + antsDefs + builtinDefs + 600 + 120);
}

// INV-15
TEST(SessionCost, Inv15NoShapeSaysWhy) {
    QTemporaryDir dir;
    const QString bare = dir.filePath("bare.jsonl");
    ASSERT_TRUE(writeFile(bare, line(assistant("m1", 1, 100, 0, 1))));
    const QJsonObject r = SessionCost::analyse(bare, 8);
    EXPECT_TRUE(r.value("startup").isNull());
    EXPECT_EQ(r.value("rent").toObject().value("definitions").toInt(), 0);
    const QJsonArray notices = r.value("notices").toArray();
    ASSERT_EQ(notices.size(), 1);
    EXPECT_TRUE(notices.at(0).toString().contains("api-request-shape"));

    RentFixture f;
    ASSERT_TRUE(f.write());
    EXPECT_FALSE(SessionCost::analyse(f.path, 8).contains("notices"));
}

TEST(SessionCost, Inv6Registration) {
    const auto slurp = [](const char *rel) {
        std::ifstream f(std::string(ANTS_SRC_DIR) + rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string reg = slurp("/mcptoolregistry.cpp");
    const std::string ci = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(reg.empty());
    ASSERT_FALSE(ci.empty());
    const auto at = reg.find("registerToolProvider(\"session_cost\"");
    ASSERT_NE(at, std::string::npos);
    // The registration statement, up to its terminating `;`.
    const std::string entry =
        ants_test::regionBetween(reg, "registerToolProvider(\"session_cost\"", ";");
    ASSERT_FALSE(entry.empty());
    EXPECT_NE(entry.find("CallerCwdContract::Required"), std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"session_cost\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"session_cost\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    EXPECT_NE(ci.substr(schema, end - schema).find("schema[\"additionalProperties\"] = false;"),
              std::string::npos);
}
