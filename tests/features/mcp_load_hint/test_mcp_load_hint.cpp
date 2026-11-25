// Feature-conformance test for ANTS-5736 — the initialize reply names the
// Ants tools a project's recent sessions loaded. Contract:
// tests/features/mcp_load_hint/spec.md

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "claudeintegration.h"
#include "mcploadhint.h"
#include "mcpprojection.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

// One transcript line carrying a ToolSearch select of `names`.
QByteArray selectLine(const QByteArray &names) {
    return R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":"ToolSearch","input":{"query":"select:)"
           + names + "\"}}]}}\n";
}

// Writes a transcript and dates it `age` seconds into the past, so the
// newest-first window is under the test's control.
void writeTranscript(const QString &dir, const QString &name,
                     const QByteArray &body, int age) {
    QFile f(dir + QLatin1Char('/') + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(body);
    // Flushed first: a buffered write landing at close() would stamp "now".
    ASSERT_TRUE(f.flush());
    ASSERT_TRUE(f.setFileTime(QDateTime::currentDateTime().addSecs(-age),
                              QFileDevice::FileModificationTime));
}

QJsonObject initialize(ClaudeIntegration &ci) {
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})", &out);
    return QJsonDocument::fromJson(line).object().value(QStringLiteral("result")).toObject();
}

}  // namespace

TEST(McpLoadHint, TranscriptDirFollowsClaudeCodesSlug) {
    ants_test::XdgGuard guard;
    guard.setEnv("CLAUDE_CONFIG_DIR", "/cfg");
    EXPECT_EQ(mcp::transcriptDirFor(QStringLiteral("/mnt/Games/Ants_Terminal.x")),
              QStringLiteral("/cfg/projects/-mnt-Games-Ants-Terminal-x"));
    guard.unsetEnv("CLAUDE_CONFIG_DIR");
    EXPECT_EQ(mcp::transcriptDirFor(QStringLiteral("/p")),
              QDir::homePath() + QStringLiteral("/.claude/projects/-p"));
}

TEST(McpLoadHint, RanksToolsBySessionsThatLoadedThem) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    // Newest first: a, b, c. e is the oldest, outside a three-file window,
    // and is the only one loading mcp__ants__spec_log.
    writeTranscript(dir, QStringLiteral("a.jsonl"),
                    selectLine("mcp__ants__roadmap_log,mcp__ants__roadmap_query")
                        + selectLine("mcp__ants__roadmap_log"), 10);
    writeTranscript(dir, QStringLiteral("b.jsonl"),
                    selectLine("mcp__ants__roadmap_log, mcp__ants__changelog_log"), 20);
    writeTranscript(dir, QStringLiteral("c.jsonl"),
                    selectLine("mcp__ants__retired_verb,mcp__other__WebFetch,Monitor"), 30);
    writeTranscript(dir, QStringLiteral("e.jsonl"), selectLine("mcp__ants__spec_log"), 50);
    const QSet<QString> known{QStringLiteral("roadmap_log"), QStringLiteral("roadmap_query"),
                              QStringLiteral("changelog_log"), QStringLiteral("spec_log")};

    EXPECT_EQ(mcp::recentlyLoadedTools(dir, known, 3, 8),
              (QStringList{QStringLiteral("mcp__ants__roadmap_log"),
                           QStringLiteral("mcp__ants__changelog_log"),
                           QStringLiteral("mcp__ants__roadmap_query")}))
        << "two sessions beat one however often one session loaded it; "
           "ties rank by name; unknown names and the old file are left out";
    EXPECT_EQ(mcp::recentlyLoadedTools(dir, known, 3, 1),
              QStringList{QStringLiteral("mcp__ants__roadmap_log")});
    EXPECT_TRUE(mcp::recentlyLoadedTools(dir + QStringLiteral("/none"), known).isEmpty());
    EXPECT_TRUE(mcp::loadHintText({}).isEmpty());
    EXPECT_TRUE(mcp::loadHintText({QStringLiteral("mcp__ants__roadmap_log")})
                    .contains(QStringLiteral("\"select:mcp__ants__roadmap_log\"")));
}

TEST(McpLoadHint, InitializeCarriesTheHintOnlyWhenThereIsOne) {
    ClaudeIntegration ci;
    EXPECT_FALSE(initialize(ci).contains(QStringLiteral("instructions")))
        << "the default sends no instructions";
    ci.setInitializeInstructions(QStringLiteral("load these"));
    EXPECT_EQ(initialize(ci).value(QStringLiteral("instructions")).toString(),
              QStringLiteral("load these"));
    ci.setInitializeInstructions(QString());
    EXPECT_FALSE(initialize(ci).contains(QStringLiteral("instructions")))
        << "empty text sends none";
}
