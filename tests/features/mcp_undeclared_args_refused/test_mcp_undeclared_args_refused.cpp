// ANTS-4659 — an undeclared MCP argument is refused; `compact` works everywhere.
// Contract: tests/features/mcp_undeclared_args_refused/spec.md.

#include "claudeintegration.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// A verb outside the default-compaction table, whose own schema writes no
// `compact`. The survey's most frequent undeclared argument was sent to it.
constexpr QLatin1String kVerb("doc_lint");

// What the stand-in handler answers: two fields compaction drops, two it keeps.
const char kBody[] = R"({"ok":true,"blank":"","flag":false,"count":0,"kept":"x"})";

struct Server {
    ClaudeIntegration ci;
    int handlerRuns = 0;

    explicit Server(const QString &verb = kVerb) {
        ci.registerToolProvider(verb, ClaudeIntegration::callerCwdContractFor(verb),
            ClaudeIntegration::ToolHandler([this](const QJsonObject &) -> QString {
                ++handlerRuns;
                return QString::fromLatin1(kBody);
            }));
    }

    QByteArray send(const QJsonObject &request) {
        QByteArray line;
        McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                            [] { return true; });
        ci.handleMcpLine(QJsonDocument(request).toJson(QJsonDocument::Compact), &out);
        return line;
    }

    void listTools() {
        ASSERT_FALSE(send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}).isEmpty());
    }

    // The verb's envelope: the text content, with any wrapper around the JSON cut.
    QJsonObject call(const QString &verb, QJsonObject args) {
        if (!args.contains("caller_cwd"))
            args["caller_cwd"] = QStringLiteral(ANTS_SOURCE_DIR);
        const QByteArray line = send(
            {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
             {"params", QJsonObject{{"name", verb}, {"arguments", args}}}});
        const QString text = QJsonDocument::fromJson(line).object()
            .value("result").toObject().value("content").toArray().at(0)
            .toObject().value("text").toString();
        const int from = text.indexOf(QLatin1Char('{'));
        const int to = text.lastIndexOf(QLatin1Char('}'));
        EXPECT_TRUE(from >= 0 && to > from) << "no envelope in: " << line.toStdString();
        return QJsonDocument::fromJson(text.mid(from, to - from + 1).toUtf8()).object();
    }
};

void expectRefused(const QJsonObject &env, const QStringList &keys) {
    const std::string shown = QJsonDocument(env).toJson().toStdString();
    ASSERT_TRUE(env.value("ok").isBool()) << shown;
    EXPECT_FALSE(env.value("ok").toBool()) << shown;
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args")) << shown;
    EXPECT_EQ(env.value("unknown_args").toArray(), QJsonArray::fromStringList(keys)) << shown;
    for (const QString &k : keys)
        EXPECT_TRUE(env.value("error").toString().contains(k))
            << "error must name " << k.toStdString() << ": " << shown;
    EXPECT_FALSE(env.contains("ignored_args")) << shown;
}

void expectRan(const QJsonObject &env) {
    ASSERT_TRUE(env.value("ok").isBool()) << QJsonDocument(env).toJson().toStdString();
    EXPECT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().toStdString();
    EXPECT_EQ(env.value("kept").toString(), QStringLiteral("x"));
}

}  // namespace

// INV-1
TEST(McpUndeclaredArgsRefused, Inv1UndeclaredArgumentRefusesNamingEveryKey) {
    Server s;
    s.listTools();
    expectRefused(s.call(kVerb, {{"zz_second", 1}, {"aa_first", true}}),
                  {QStringLiteral("aa_first"), QStringLiteral("zz_second")});
}

// INV-2
TEST(McpUndeclaredArgsRefused, Inv2RefusedCallNeverRunsTheVerb) {
    Server s;
    s.listTools();
    expectRefused(s.call(kVerb, {{"not_an_argument", 1}}),
                  {QStringLiteral("not_an_argument")});
    EXPECT_EQ(s.handlerRuns, 0) << "the verb ran before the refusal";
    expectRan(s.call(kVerb, {}));
    EXPECT_EQ(s.handlerRuns, 1) << "control: the same call without the argument runs";
}

// INV-3
TEST(McpUndeclaredArgsRefused, Inv3UniversalArgumentsAreNeverRefused) {
    Server s;
    s.listTools();
    const QJsonObject env = s.call(kVerb, {
        {"encoding", "json"},
        {"fields", QJsonArray{"ok", "kept", "blank", "flag"}},
        {"compact", false}});
    expectRan(env);
    EXPECT_EQ(s.handlerRuns, 1);
}

// INV-4
TEST(McpUndeclaredArgsRefused, Inv4CompactWorksOutsideTheDefaultTable) {
    Server s;
    s.listTools();

    const QJsonObject compacted = s.call(kVerb, {{"compact", true}});
    expectRan(compacted);
    EXPECT_FALSE(compacted.contains("blank")) << "compact:true keeps an empty string";
    EXPECT_FALSE(compacted.contains("flag")) << "compact:true keeps a false";
    ASSERT_TRUE(compacted.value("count").isDouble()) << "a number must survive";
    EXPECT_FALSE(compacted.contains("ignored_args"));

    for (const QJsonObject &args : {QJsonObject{}, QJsonObject{{"compact", false}}}) {
        const QJsonObject whole = s.call(kVerb, args);
        expectRan(whole);
        ASSERT_TRUE(whole.value("flag").isBool())
            << "an unasked compaction folded a field away";
        EXPECT_TRUE(whole.contains("blank"));
    }
}

// INV-5
TEST(McpUndeclaredArgsRefused, Inv5ToolInfoShowsCompactOnEveryVerb) {
    Server s;
    s.listTools();
    const QJsonObject props = s.call(QStringLiteral("tool_info"), {{"name", kVerb}})
        .value("inputSchema").toObject().value("properties").toObject();
    ASSERT_TRUE(props.contains("checks")) << "precondition: this is doc_lint's schema";
    ASSERT_TRUE(props.value("compact").isObject());
    EXPECT_EQ(props.value("compact").toObject().value("type").toString(),
              QStringLiteral("boolean"));
}

// INV-6
TEST(McpUndeclaredArgsRefused, Inv6UnhonouredDispatchArgumentsAreRefused) {
    Server s;
    s.listTools();
    // doc_lint honours `offload` and not `etag_match`.
    expectRan(s.call(kVerb, {{"offload", false}}));
    s.handlerRuns = 0;
    expectRefused(s.call(kVerb, {{"etag_match", "abc"}}),
                  {QStringLiteral("etag_match")});
    EXPECT_EQ(s.handlerRuns, 0);
}

// INV-7
TEST(McpUndeclaredArgsRefused, Inv7VerbWithoutAKnownSchemaIsNotRefused) {
    const QString unlisted = QStringLiteral("zz_test_no_schema");
    Server s(unlisted);
    s.listTools();
    expectRan(s.call(unlisted, {{"anything", 1}}));
    EXPECT_EQ(s.handlerRuns, 1);

    Server neverListed;
    expectRan(neverListed.call(kVerb, {{"anything", 1}}));
    EXPECT_EQ(neverListed.handlerRuns, 1);
}
