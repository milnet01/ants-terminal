// Feature-conformance test for ANTS-2158 — eager-load the highest-frequency
// MCP verbs so they escape Claude Code's tool-search deferral (callable
// without a ToolSearch hop). The tools/list builder isn't invokable in
// isolation, so this is a source-grep wiring contract over the marking
// pass in claudeintegration.cpp (the shape other wiring tests use).
// See tests/features/mcp_eager_load/spec.md + ROADMAP ANTS-2158.

#include <gtest/gtest.h>
#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "claudeintegration.h"
#include "mcpprojection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>

ANTS_TEST_SCOPE();

namespace {
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
}  // namespace

TEST(McpEagerLoad, AlwaysLoadMarkingWired) {
    const std::string ci =
        ants_test::slurpClaudeIntegration();

    // The exact, version-honoured field name + placement (Claude Code
    // v2.1.121+, code.claude.com/docs/en/mcp.md).
    EXPECT_TRUE(has(ci, "anthropic/alwaysLoad"))
        << "eager-load _meta field missing from tools/list builder";
    EXPECT_TRUE(has(ci, "kEagerVerbs"));

    // The curated set itself, read from its initializer: the grep / Read
    // substitutes are in it, and the rarely called write verbs are not —
    // each member costs context at every session start.
    const auto open = ci.find("kEagerVerbs = {");
    ASSERT_NE(open, std::string::npos);
    const auto close = ci.find("};", open);
    ASSERT_NE(close, std::string::npos);
    const std::string set = ci.substr(open, close - open);
    for (const char *verb : {"\"workspace_search\"", "\"find_definition\"",
                             "\"file_outline\"", "\"read_region\""}) {
        EXPECT_TRUE(has(set, verb)) << "missing eager verb: " << verb;
    }
    for (const char *verb : {"\"roadmap_log\"", "\"changelog_log\""}) {
        EXPECT_FALSE(has(set, verb))
            << "always-loaded but rarely called: " << verb;
    }
}

namespace {
QJsonObject mcpCall(ClaudeIntegration &ci, const QByteArray &request) {
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(request, &out);
    return QJsonDocument::fromJson(line).object()
        .value(QStringLiteral("result")).toObject();
}
}  // namespace

// ANTS-5628 — an always-loaded verb's argument descriptions are one line on
// the wire, and tool_info still serves the full text.
TEST(McpEagerLoad, Ants5628WireArgumentsAreShort) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());

    constexpr int kMaxArgDescription = 140;
    int eager = 0;
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        if (!t.value(QStringLiteral("_meta")).toObject()
                 .value(QStringLiteral("anthropic/alwaysLoad")).toBool())
            continue;
        ++eager;
        const QString name = t.value(QStringLiteral("name")).toString();
        const QJsonObject props = t.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const int len = it.value().toObject()
                .value(QStringLiteral("description")).toString().size();
            EXPECT_LE(len, kMaxArgDescription)
                << name.toStdString() << "/" << it.key().toStdString()
                << " is " << len << " chars on the wire";
        }
    }
    EXPECT_EQ(eager, 4) << "the four always-loaded verbs";

    // The full text is one tool_info call away.
    const QJsonObject info = mcpCall(ci,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"workspace_search"}}})");
    const QString text = info.value(QStringLiteral("content")).toArray()
        .at(0).toObject().value(QStringLiteral("text")).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("the rg scan still runs")))
        << "tool_info no longer carries count_only's full description";
}

// ANTS-5271 — the five large roadmap and changelog verbs are deferred, not
// always-loaded, but a session that loads one pays its whole schema for the
// rest of the session. On the wire each argument description, nested item
// fields included, is one line; tool_info still serves the full text.
// ANTS-4931 — six more verbs with long argument prose join them.
TEST(McpEagerLoad, Ants5271LeanVerbArgumentsAreShort) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());

    constexpr int kMaxArgDescription = 140;
    const QStringList lean = {
        QStringLiteral("roadmap_log"), QStringLiteral("roadmap_query"),
        QStringLiteral("feedback_log"), QStringLiteral("changelog_log"),
        QStringLiteral("roadmap_migrate"), QStringLiteral("audit_run"),
        QStringLiteral("apply_edits"), QStringLiteral("doc_citations"),
        QStringLiteral("doc_lint"), QStringLiteral("spec_log"),
        QStringLiteral("invariant_check")};
    int seen = 0;
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString();
        if (!lean.contains(name)) continue;
        ++seen;
        const QJsonObject props = t.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
            const QJsonObject p = it.value().toObject();
            EXPECT_LE(p.value(QStringLiteral("description")).toString().size(),
                      kMaxArgDescription)
                << name.toStdString() << "/" << it.key().toStdString();
            const QJsonObject inner = p.value(QStringLiteral("items"))
                .toObject().value(QStringLiteral("properties")).toObject();
            for (auto jt = inner.constBegin(); jt != inner.constEnd(); ++jt) {
                EXPECT_LE(jt.value().toObject()
                              .value(QStringLiteral("description"))
                              .toString().size(),
                          kMaxArgDescription)
                    << name.toStdString() << "/" << it.key().toStdString()
                    << "." << jt.key().toStdString();
            }
        }
    }
    EXPECT_EQ(seen, lean.size()) << "every lean verb is registered";

    // The full text is one tool_info call away.
    const auto infoText = [&ci](const char *verb) {
        return mcpCall(ci, QStringLiteral(
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"%1"}}})")
                .arg(QLatin1String(verb)).toUtf8())
            .value(QStringLiteral("content")).toArray()
            .at(0).toObject().value(QStringLiteral("text")).toString();
    };
    EXPECT_TRUE(infoText("roadmap_log").contains(
        QStringLiteral("the preview is not a simulation")))
        << "tool_info no longer carries roadmap_log dry_run's full description";
    EXPECT_TRUE(infoText("audit_run").contains(
        QStringLiteral("nothing to audit")))
        << "tool_info no longer carries audit_run scope's full description";
}

// ANTS-5694 — the four always-loaded verbs keep a short description on the
// wire: purpose, the one usage trap, and the tool_info pointer. The two
// etag verbs keep a one-line "Etag tip:" (mcp_etag_tip_memo INV-1), and
// tool_info still serves the long text.
TEST(McpEagerLoad, Ants5694EagerDescriptionsAreShort) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());

    constexpr int kMaxDescriptionBytes = 400;
    const QStringList etag = {QStringLiteral("file_outline"),
                              QStringLiteral("read_region")};
    int eager = 0;
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        if (!t.value(QStringLiteral("_meta")).toObject()
                 .value(QStringLiteral("anthropic/alwaysLoad")).toBool())
            continue;
        ++eager;
        const QString name = t.value(QStringLiteral("name")).toString();
        const QString desc = t.value(QStringLiteral("description")).toString();
        EXPECT_LE(desc.toUtf8().size(), kMaxDescriptionBytes)
            << name.toStdString() << " description is "
            << desc.toUtf8().size() << " bytes on the wire";
        EXPECT_TRUE(desc.contains(
            QStringLiteral("tool_info {name:\"%1\"}").arg(name)))
            << name.toStdString() << " lost its tool_info pointer";
        EXPECT_EQ(desc.contains(QStringLiteral("Etag tip:")),
                  etag.contains(name))
            << name.toStdString() << " Etag tip presence";
    }
    EXPECT_EQ(eager, 4) << "the four always-loaded verbs";

    const QJsonObject info = mcpCall(ci,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"workspace_search"}}})");
    const QString text = info.value(QStringLiteral("content")).toArray()
        .at(0).toObject().value(QStringLiteral("text")).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("says why in truncated_reason")))
        << "tool_info no longer carries workspace_search's full description";
}

// ANTS-5714 — five arguments mean the same on every verb, and their long
// notes were repeated on each one. On the wire every verb carries one line
// for fields, compact, encoding and etag_match, and the generic caller_cwd
// wording is one line too; a verb-specific caller_cwd wording is kept.
// tool_info still serves the full text.
TEST(McpEagerLoad, Ants5714SharedArgumentsAreShortOnEveryVerb) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(tools.isEmpty());

    constexpr int kMaxArgDescription = 140;
    const QStringList shared = {
        QStringLiteral("fields"), QStringLiteral("compact"),
        QStringLiteral("encoding"), QStringLiteral("etag_match")};
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString();
        const QJsonObject props = t.value(QStringLiteral("inputSchema"))
            .toObject().value(QStringLiteral("properties")).toObject();
        for (const QString &arg : shared) {
            if (!props.contains(arg)) continue;
            const int len = props.value(arg).toObject()
                .value(QStringLiteral("description")).toString().size();
            EXPECT_LE(len, kMaxArgDescription)
                << name.toStdString() << "/" << arg.toStdString()
                << " is " << len << " chars on the wire";
        }
        EXPECT_FALSE(props.value(QStringLiteral("caller_cwd")).toObject()
                         .value(QStringLiteral("description")).toString()
                         .contains(QStringLiteral("Required by the dispatcher")))
            << name.toStdString() << " sends the generic long caller_cwd text";
    }

    const QJsonObject info = mcpCall(ci,
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"git_state"}}})");
    const QString text = info.value(QStringLiteral("content")).toArray()
        .at(0).toObject().value(QStringLiteral("text")).toString();
    EXPECT_TRUE(text.contains(QStringLiteral("Required by the dispatcher")))
        << "tool_info no longer carries caller_cwd's full description";
    EXPECT_TRUE(text.contains(QStringLiteral("fields_available")))
        << "tool_info no longer carries fields' full description";
}

// ANTS-6325 — Claude Code sends an argument the wire schema leaves out as text,
// so "20" and "true" must reach the verb as the types the full schema declares.
TEST(McpEagerLoad, Ants6325HiddenArgumentSentAsTextIsHonoured) {
    const QJsonObject props = QJsonDocument::fromJson(R"({
        "timeout_sec": {"type":"integer"}, "ratio": {"type":"number"},
        "include_hidden": {"type":"boolean"}, "fields": {"type":"array"},
        "opts": {"type":"object"}, "pattern": {"type":"string"},
        "either": {"type":["string","array"]}})").object();
    const QJsonObject in = QJsonDocument::fromJson(R"({
        "timeout_sec":"20", "ratio":"0.5", "include_hidden":"true", "fields":"[\"a\"]",
        "opts":"{\"k\":1}", "pattern":"7", "either":"[1]", "unknown":"3"})").object();
    const QJsonObject out = mcp::coerceArgTypes(in, props);
    EXPECT_EQ(out.value(QStringLiteral("timeout_sec")).toInt(), 20);
    EXPECT_EQ(out.value(QStringLiteral("ratio")).toDouble(), 0.5);
    EXPECT_EQ(out.value(QStringLiteral("include_hidden")), QJsonValue(true));
    EXPECT_EQ(out.value(QStringLiteral("fields")).toArray().size(), 1);
    EXPECT_EQ(out.value(QStringLiteral("opts")).toObject().value(QStringLiteral("k")).toInt(), 1);
    // A string, a union type and an undeclared argument stay as sent.
    for (const char *same : {"pattern", "either", "unknown"})
        EXPECT_EQ(out.value(QLatin1String(same)), in.value(QLatin1String(same))) << same;

    // Text that is not the declared type is left for the verb to judge.
    const QJsonObject bad = QJsonDocument::fromJson(R"({
        "timeout_sec":"soon", "ratio":"", "include_hidden":"maybe", "fields":"{}", "opts":"[]"})").object();
    EXPECT_EQ(mcp::coerceArgTypes(bad, props), bad);

    // The dispatcher applies it to every call, from the saved full schema.
    const std::string ci = ants_test::slurpClaudeIntegration();
    EXPECT_NE(ci.find("mcp::coerceArgTypes(params.value(\"arguments\").toObject(),"), std::string::npos);
}

// ANTS-5725 — an always-loaded verb's wire schema leaves out the arguments
// used in under 5% of its calls (user decision, 2026-10-16). Each is still
// accepted: tool_info {name} lists it, and passing it raises no ignored_args.
// Measured with tools/mcp-arg-usage-survey.py on 2026-10-22.
TEST(McpEagerLoad, Ants5725RareArgumentsLeaveTheWire) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    const auto wireProps = [&](const QString &verb) {
        for (const auto &v : tools)
            if (v.toObject().value(QStringLiteral("name")).toString() == verb)
                return v.toObject().value(QStringLiteral("inputSchema")).toObject()
                    .value(QStringLiteral("properties")).toObject();
        return QJsonObject{};
    };
    const struct { const char *verb; QStringList hidden, kept; } rows[] = {
        {"workspace_search",
         {"case", "count_only", "dedup", "encoding", "fields", "include_hidden",
          "match_wrapped", "matches_only", "max_bytes", "multiline", "offset", "query", "raw",
          "respect_gitignore", "timeout_sec"},
         {"pattern", "caller_cwd", "regex", "glob", "headline_only", "lane",
          "max_results", "max_match_bytes", "context", "exclude_glob",
          "enclosing_symbol", "files_only"}},
        {"file_outline", {"mode", "etag_match", "etags", "max_bytes", "raw"},
         {"path", "paths", "include_doc_comment", "compact", "sizes",
          "max_heading_level", "fields", "max_symbols", "encoding", "filter"}},
        {"read_region",
         {"call_sequence", "compact", "etag_match", "fields", "max_bytes", "raw", "tail_lines"},
         {"path", "start_line", "end_line", "section", "max_line_bytes", "symbol"}},
    };
    for (const auto &row : rows) {
        const QString verb = QString::fromLatin1(row.verb);
        const QJsonObject props = wireProps(verb);
        ASSERT_FALSE(props.isEmpty()) << row.verb;
        for (const QString &a : row.hidden)
            EXPECT_FALSE(props.contains(a)) << row.verb << "/" << a.toStdString()
                                            << " is still on the wire";
        for (const QString &a : row.kept)
            EXPECT_TRUE(props.contains(a)) << row.verb << "/" << a.toStdString()
                                           << " was hidden but is used in 5% or more of calls";

        const QJsonObject info = mcpCall(ci, QStringLiteral(
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"%1"}}})")
            .arg(verb).toStdString().c_str());
        const QString text = info.value(QStringLiteral("content")).toArray()
            .at(0).toObject().value(QStringLiteral("text")).toString();
        for (const QString &a : row.hidden)
            EXPECT_TRUE(text.contains(QLatin1Char('"') + a + QLatin1Char('"')))
                << "tool_info " << row.verb << " no longer lists " << a.toStdString();
    }

    // Hidden is not ignored. The ignored_args advisory and doc_facts read the
    // accepted-argument set, which tools/list builds from the full schema,
    // so every hidden argument must still be in it.
    const QHash<QString, QSet<QString>> accepted = ci.verbArgsSnapshot();
    for (const auto &row : rows)
        for (const QString &a : row.hidden)
            EXPECT_TRUE(accepted.value(QString::fromLatin1(row.verb)).contains(a))
                << row.verb << "/" << a.toStdString() << " would raise ignored_args";
}

namespace {
// tool_info {name}'s reply text: the verb's full descriptor as JSON.
QString toolInfoText(ClaudeIntegration &ci, const QString &verb) {
    return mcpCall(ci, QStringLiteral(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"tool_info","arguments":{"name":"%1"}}})")
        .arg(verb).toUtf8())
        .value(QStringLiteral("content")).toArray()
        .at(0).toObject().value(QStringLiteral("text")).toString();
}
QJsonObject toolInfoSchema(ClaudeIntegration &ci, const QString &verb) {
    return QJsonDocument::fromJson(toolInfoText(ci, verb).toUtf8()).object()
        .value(QStringLiteral("inputSchema")).toObject();
}
}  // namespace

// ANTS-6626 — get_text, recent_errors and last_selection take a `tab` index
// in place of caller_cwd, so their wire caller_cwd does not call it required
// alone.
TEST(McpEagerLoad, Ants6626TabVerbsCallerCwdNamesTheTab) {
    ClaudeIntegration ci;
    const QJsonArray tools =
        mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
            .value(QStringLiteral("tools")).toArray();
    int seen = 0;
    for (const auto &v : tools) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString();
        if (name != QLatin1String("get_text") && name != QLatin1String("recent_errors")
            && name != QLatin1String("last_selection"))
            continue;
        ++seen;
        const QString desc = t.value(QStringLiteral("inputSchema")).toObject()
            .value(QStringLiteral("properties")).toObject()
            .value(QStringLiteral("caller_cwd")).toObject()
            .value(QStringLiteral("description")).toString();
        EXPECT_TRUE(desc.contains(QStringLiteral("`tab`")))
            << name.toStdString() << "/caller_cwd: " << desc.toStdString();
        EXPECT_FALSE(desc.contains(QStringLiteral("(required)")))
            << name.toStdString() << "/caller_cwd: " << desc.toStdString();
    }
    EXPECT_EQ(seen, 3);
}

// ANTS-6626 — each schema says what its verb accepts: an argument with a
// declared alias is not in required[]; caller_cwd is, where the dispatcher
// refuses without it; an alias takes the same shapes as its argument; every
// argument is described; and an em dash is an em dash.
TEST(McpEagerLoad, Ants6626SchemasMatchWhatTheVerbAccepts) {
    ClaudeIntegration ci;
    ASSERT_FALSE(mcpCall(ci, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})")
                     .value(QStringLiteral("tools")).toArray().isEmpty());
    const auto required = [&](const QString &verb) {
        QStringList out;
        for (const auto &v : toolInfoSchema(ci, verb).value(QStringLiteral("required")).toArray())
            out << v.toString();
        return out;
    };

    const QJsonObject ws = toolInfoSchema(ci, QStringLiteral("workspace_search"));
    ASSERT_TRUE(ws.value(QStringLiteral("properties")).toObject().contains(QStringLiteral("query")));
    EXPECT_FALSE(required(QStringLiteral("workspace_search")).contains(QStringLiteral("pattern")))
        << "a call sending only the alias `query` fails a strict schema check";

    ASSERT_TRUE(toolInfoSchema(ci, QStringLiteral("find_sources"))
                    .value(QStringLiteral("properties")).toObject()
                    .contains(QStringLiteral("symbol")));
    EXPECT_FALSE(required(QStringLiteral("find_sources")).contains(QStringLiteral("topic")));
    EXPECT_TRUE(required(QStringLiteral("find_sources")).contains(QStringLiteral("caller_cwd")));
    EXPECT_TRUE(required(QStringLiteral("cold_eyes_brief")).contains(QStringLiteral("caller_cwd")));
    EXPECT_TRUE(required(QStringLiteral("cold_eyes_brief")).contains(QStringLiteral("lane")));

    const QJsonObject rq = toolInfoSchema(ci, QStringLiteral("roadmap_query"))
                               .value(QStringLiteral("properties")).toObject();
    ASSERT_TRUE(rq.value(QStringLiteral("status")).toObject().value(QStringLiteral("anyOf")).isArray());
    EXPECT_EQ(rq.value(QStringLiteral("filter")).toObject().value(QStringLiteral("anyOf")),
              rq.value(QStringLiteral("status")).toObject().value(QStringLiteral("anyOf")))
        << "`filter` must take an array of lifecycles, as `status` does";

    for (const char *verb : {"test_audit_partition", "test_audit_brief"}) {
        const QJsonObject props = toolInfoSchema(ci, QString::fromLatin1(verb))
                                      .value(QStringLiteral("properties")).toObject();
        ASSERT_FALSE(props.isEmpty()) << verb;
        for (auto it = props.constBegin(); it != props.constEnd(); ++it)
            EXPECT_FALSE(it.value().toObject().value(QStringLiteral("description"))
                             .toString().isEmpty())
                << verb << "/" << it.key().toStdString() << " has no description";
    }

    // The reply is JSON text: a real em dash is the character itself, a
    // literal backslash-u sequence shows as `\\u2014`.
    for (const char *verb : {"changelog_log", "indie_review_corroborate"}) {
        const QString text = toolInfoText(ci, QString::fromLatin1(verb));
        ASSERT_TRUE(text.contains(QChar(0x2014))) << verb;
        EXPECT_FALSE(text.contains(QStringLiteral("\\\\u2014")))
            << verb << " sends the six characters \\u2014 for an em dash";
    }
}
