// ANTS-6365 — a caller that sends an argument and its alias together is
// refused bad_op_combo with both names in supplied[] (mcp-tools.md, schema
// hygiene). Before, each of these verbs used one name and dropped the other,
// which hid the caller's mistake. ANTS-6360 fixed invariant_check and
// similar_code; this covers the rest.
//
// Driven, not scraped: each handler is called with both names and must refuse
// before doing any work.

#include "remotecontrol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <functional>

#include <gtest/gtest.h>

namespace {

using Handler = std::function<QJsonDocument(RemoteControl &, const QJsonObject &)>;

void expectClash(const Handler &call, QJsonObject req, const QStringList &names) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    req[QStringLiteral("caller_cwd")] = tmp.path();
    RemoteControl rc(nullptr);
    const QJsonObject r = call(rc, req).object();
    const QString label = QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool(true)) << label.toStdString();
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_op_combo"))
        << label.toStdString() << " -> "
        << QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString();
    QStringList supplied;
    for (const auto &v : r.value(QStringLiteral("supplied")).toArray())
        supplied << v.toString();
    EXPECT_EQ(supplied, names) << label.toStdString();
}

QJsonObject obj(std::initializer_list<std::pair<QString, QJsonValue>> kv) {
    QJsonObject o;
    for (const auto &p : kv) o[p.first] = p.second;
    return o;
}

}  // namespace

TEST(McpAliasClash, RoadmapQuery) {
    const Handler h = [](RemoteControl &rc, const QJsonObject &r) { return rc.cmdRoadmapQuery(r); };
    expectClash(h, obj({{"status", "all"}, {"filter", "all"}}), {"status", "filter"});
    expectClash(h, obj({{"query", "a"}, {"q", "a"}}), {"query", "q"});
    expectClash(h, obj({{"limit", 5}, {"max_results", 5}}), {"limit", "max_results"});
}

TEST(McpAliasClash, WorkspaceSearch) {
    expectClash([](RemoteControl &rc, const QJsonObject &r) { return rc.cmdWorkspaceSearch(r); },
                obj({{"pattern", "x"}, {"query", "x"}}), {"pattern", "query"});
}

TEST(McpAliasClash, FileOutline) {
    expectClash([](RemoteControl &rc, const QJsonObject &r) { return rc.cmdFileOutline(r); },
                obj({{"path", "a.cpp"}, {"file_path", "a.cpp"}}), {"path", "file_path"});
}

TEST(McpAliasClash, CodebaseIndex) {
    expectClash([](RemoteControl &rc, const QJsonObject &r) { return rc.cmdCodebaseIndex(r); },
                obj({{"file_path", "a.cpp"}, {"path", "a.cpp"}}), {"file_path", "path"});
}

TEST(McpAliasClash, CoChangeFamily) {
    expectClash([](RemoteControl &rc, const QJsonObject &r) { return rc.cmdCoChangeFamily(r); },
                obj({{"stems", QJsonArray{"x"}}, {"stem", "x"}}), {"stems", "stem"});
}

TEST(McpAliasClash, FindSources) {
    expectClash([](RemoteControl &rc, const QJsonObject &r) { return rc.cmdFindSources(r); },
                obj({{"topic", "x"}, {"symbol", "x"}}), {"topic", "symbol"});
}

TEST(McpAliasClash, ApplyEditsPerEdit) {
    const Handler h = [](RemoteControl &rc, const QJsonObject &r) { return rc.cmdApplyEdits(r); };
    const auto edits = [](const QJsonObject &e) {
        return obj({{"edits", QJsonArray{e}}});
    };
    expectClash(h, edits(obj({{"path", "f"}, {"old", "a"}, {"old_string", "a"}, {"new", "b"}})),
                {"old", "old_string"});
    expectClash(h, edits(obj({{"path", "f"}, {"old", "a"}, {"new", "b"}, {"new_text", "b"}})),
                {"new", "new_text"});
    // All three spellings of one argument: every name sent is listed.
    expectClash(h, edits(obj({{"path", "f"}, {"old_string", "a"}, {"old_text", "a"},
                              {"old", "a"}, {"new", "b"}})),
                {"old", "old_string", "old_text"});
}
