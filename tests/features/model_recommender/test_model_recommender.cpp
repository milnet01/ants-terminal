// Feature-conformance test for ANTS-1226 — ModelRecommender scorer.
// See tests/features/model_recommender/spec.md.

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QTextStream>
#include <fstream>
#include <sstream>
#include <string>
#include "modelrecommender.h"

#ifndef SRC_MODELRECOMMENDER_CPP_PATH
#error "SRC_MODELRECOMMENDER_CPP_PATH compile definition required"
#endif
#ifndef SRC_MODELRECOMMENDER_H_PATH
#error "SRC_MODELRECOMMENDER_H_PATH compile definition required"
#endif

namespace {
std::string slurpFile(const char *path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(2); }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Slice the body of a brace-balanced function definition starting at the
// marker through to its matching closing brace at column 1.
std::string functionBody(const std::string &src, const std::string &marker) {
    const auto headerStart = src.find(marker);
    if (headerStart == std::string::npos) return {};
    const auto openBrace = src.find('{', headerStart);
    if (openBrace == std::string::npos) return {};
    int depth = 1;
    size_t i = openBrace + 1;
    for (; i < src.size() && depth > 0; ++i) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
    }
    return src.substr(openBrace, i - openBrace);
}
}  // namespace

namespace {

QString writeSyntheticTranscript(const QVector<QJsonArray> &turns,
                                  QTemporaryFile &f,
                                  const QString &model = QStringLiteral("claude-sonnet-4-6"))
{
    if (!f.open()) return f.fileName();
    QTextStream out(&f);
    for (const QJsonArray &content : turns) {
        QJsonObject turn;
        turn["type"] = "assistant";
        turn["timestamp"] = "2026-05-21T00:00:00.000Z";
        QJsonObject msg;
        msg["content"] = content;
        msg["model"] = model;
        turn["message"] = msg;
        out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    }
    f.close();
    return f.fileName();
}

// ANTS-1890 — sibling of writeSyntheticTranscript that emits N assistant
// turns followed by ONE user turn with arbitrary text. score()'s reverse
// walk finds the user line first; hasCommitIntent runs against it.
QString writeSyntheticTranscriptWithUserTail(
    const QVector<QJsonArray> &assistantTurns,
    const QString &userText,
    QTemporaryFile &f,
    const QString &model = QStringLiteral("claude-sonnet-4-6"))
{
    if (!f.open()) return f.fileName();
    QTextStream out(&f);
    for (const QJsonArray &content : assistantTurns) {
        QJsonObject turn;
        turn["type"] = "assistant";
        turn["timestamp"] = "2026-05-21T00:00:00.000Z";
        QJsonObject msg;
        msg["content"] = content;
        msg["model"] = model;
        turn["message"] = msg;
        out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    }
    {
        QJsonObject turn;
        turn["type"] = "user";
        QJsonObject msg;
        QJsonArray content;
        QJsonObject block;
        block["type"] = "text";
        block["text"] = userText;
        content.append(block);
        msg["content"] = content;
        turn["message"] = msg;
        out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    }
    f.close();
    return f.fileName();
}

QJsonObject toolUse(const QString &name) {
    QJsonObject t;
    t["type"] = "tool_use";
    t["name"] = name;
    t["id"]   = "tu_test";
    t["input"] = QJsonObject();
    return t;
}

QJsonObject textBlock(const QString &text) {
    QJsonObject t;
    t["type"] = "text";
    t["text"] = text;
    return t;
}

} // namespace

// INV-1: empty transcript → Sonnet
TEST(ModelRecommender, Inv1EmptyTranscriptReturnsSonnet) {
    QTemporaryFile f;
    ASSERT_TRUE(f.open()); f.close();
    const auto result = ModelRecommender::score(f.fileName());
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Sonnet);
}

// INV-1: missing file → Sonnet (no crash)
TEST(ModelRecommender, Inv1MissingFileReturnsSonnet) {
    const auto result =
        ModelRecommender::score(QStringLiteral("/tmp/no_such_file_ants_1226.jsonl"));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Sonnet);
}

// INV-2: plan keyword + sustained writes → Opus.
// Under ANTS-1890 v2 weighted writes (sum ≈ 40 across a full 20-turn
// window), the v1 fixture (1 turn × 4 writes = weighted 4.0 < 8) no
// longer trips many_writes. The behavioural contract — "writing-heavy
// session with plan keyword → Opus" — still holds; the fixture is
// scaled to match v2's weighted threshold by spreading writes across
// the full window.
TEST(ModelRecommender, Inv2PlanKeywordPlusManyWritesReturnsOpus) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 20; ++i) {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("Let me design the architecture for this spec.")));
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Opus)
        << "reason: " << result.reason.toStdString();
}

// INV-3: no writes, ≤2 tools → Haiku
TEST(ModelRecommender, Inv3NoWritesFewToolsReturnsHaiku) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku);
}

// INV-8: only last 20 turns scored — oldest turns don't affect result
TEST(ModelRecommender, Inv8OnlyLast20TurnsScored) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    // Turns 0-4: plan keywords + 4 writes (oldest; should be IGNORED)
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("Let me design the architecture.")));
        content.append(toolUse(QStringLiteral("Edit")));
        content.append(toolUse(QStringLiteral("Edit")));
        content.append(toolUse(QStringLiteral("Edit")));
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    // Turns 5-24: purely mechanical (most-recent 20; dominate)
    for (int i = 0; i < 20; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    // With 20 mechanical turns: score == -2 (mechanical penalty) ≤ -2 → Haiku
    // (ANTS-1930 raised the Haiku threshold from ≤ -1 to ≤ -2).
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku)
        << "Only the last 20 turns should be scored. "
           "reason: " << result.reason.toStdString();
}

// ANTS-1930: rebalanced thresholds enable upgrades at sc == +2.
// Pre-1930 a score of exactly 2 routed to Sonnet (Opus needed >= 3),
// producing a downgrade-only ratchet — users were observed never to be
// upgraded. A plan-keyword session with 3 distinct non-write tools
// scores exactly +2 (plan_keyword) with NO mechanical penalty
// (toolDiversity > 2) and must now route to Opus.
TEST(ModelRecommender, Ants1930PlanKeywordModerateWorkUpgradesToOpus) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("Let me review the plan.")));
        content.append(toolUse(QStringLiteral("Bash")));
        content.append(toolUse(QStringLiteral("Grep")));
        content.append(toolUse(QStringLiteral("Read")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Opus)
        << "sc should be +2 (plan_keyword, no mechanical penalty via "
           "toolDiversity=3); ANTS-1930 routes +2 to Opus. reason: "
        << result.reason.toStdString();
}

// ANTS-1930: a score of exactly -1 now stays Sonnet (pre-1930 it routed
// to Haiku at <= -1). Long prompts (+1) with mechanical tool use (-2)
// nets -1 — the rebalanced Haiku threshold (<= -2) keeps it on Sonnet.
TEST(ModelRecommender, Ants1930ScoreMinusOneStaysSonnet) {
    QTemporaryFile f;
    const QString longText(600, QLatin1Char('x'));   // avgLen >= 500 → +1
    QVector<QJsonArray> turns;
    for (int i = 0; i < 5; ++i) {
        QJsonArray content;
        content.append(textBlock(longText));
        content.append(toolUse(QStringLiteral("Bash")));   // no writes, 1 tool
        turns.append(content);
    }
    const auto result = ModelRecommender::score(writeSyntheticTranscript(turns, f));
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Sonnet)
        << "sc should be -1 (long_prompts +1, mechanical -2); ANTS-1930 "
           "keeps -1 on Sonnet rather than Haiku. reason: "
        << result.reason.toStdString();
}

// INV-4/INV-5: tierFromModelId maps correctly
TEST(ModelRecommender, TierFromModelId) {
    EXPECT_EQ(ModelRecommender::tierFromModelId(QStringLiteral("claude-opus-4-7")),
              ModelRecommender::Tier::Opus);
    EXPECT_EQ(ModelRecommender::tierFromModelId(QStringLiteral("claude-sonnet-4-6")),
              ModelRecommender::Tier::Sonnet);
    EXPECT_EQ(ModelRecommender::tierFromModelId(QStringLiteral("claude-haiku-4-5")),
              ModelRecommender::Tier::Haiku);
    EXPECT_EQ(ModelRecommender::tierFromModelId(QString()),
              ModelRecommender::Tier::Sonnet);
}

// model read from transcript message.model field
TEST(ModelRecommender, CurrentModelReadFromTranscript) {
    QTemporaryFile f;
    QJsonArray content;
    content.append(toolUse(QStringLiteral("Bash")));
    QVector<QJsonArray> turns;
    turns.append(content);
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f, QStringLiteral("claude-opus-4-7")));
    EXPECT_EQ(result.currentModel, QStringLiteral("claude-opus-4-7"));
}

// ANTS-1916 — a /model command newer than the last assistant turn is the
// real current model. The model field on assistant turns only updates on
// the NEXT assistant turn, so without this the chip + auto-switcher lag a
// full turn (the stale-chip + actuator-thrash bug). Helper writes a
// {type:"system",subtype:"local_command"} event mirroring Claude Code's
// on-disk shape.
namespace {
QString writeTranscriptWithModelCommand(QTemporaryFile &f,
                                        const QString &assistantModel,
                                        const QString &modelArg) {
    if (!f.open()) return f.fileName();
    QTextStream out(&f);
    {
        QJsonObject turn;
        turn["type"] = "assistant";
        QJsonObject msg;
        msg["content"] = QJsonArray{};
        msg["model"] = assistantModel;
        turn["message"] = msg;
        out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    }
    {
        QJsonObject cmd;
        cmd["type"] = "system";
        cmd["subtype"] = "local_command";
        cmd["content"] = QStringLiteral(
            "<command-name>/model</command-name>\n"
            "            <command-message>model</command-message>\n"
            "            <command-args>%1</command-args>").arg(modelArg);
        out << QJsonDocument(cmd).toJson(QJsonDocument::Compact) << "\n";
    }
    f.close();
    return f.fileName();
}
}  // namespace

TEST(ModelRecommender, ModelCommandAfterAssistantTurnWins) {
    // Last assistant turn ran on Sonnet; a /model haiku command followed.
    // currentModel must resolve to the commanded tier, not the stale Sonnet.
    QTemporaryFile f;
    const auto result = ModelRecommender::score(writeTranscriptWithModelCommand(
        f, QStringLiteral("claude-sonnet-4-6"), QStringLiteral("haiku")));
    EXPECT_EQ(ModelRecommender::tierFromModelId(result.currentModel),
              ModelRecommender::Tier::Haiku)
        << "actual currentModel=" << result.currentModel.toStdString();
}

TEST(ModelRecommender, EmptyModelCommandArgIgnored) {
    // The user opened the picker without choosing (empty args): the stale
    // assistant-turn model stands rather than blanking currentModel.
    QTemporaryFile f;
    const auto result = ModelRecommender::score(writeTranscriptWithModelCommand(
        f, QStringLiteral("claude-opus-4-7"), QString()));
    EXPECT_EQ(result.currentModel, QStringLiteral("claude-opus-4-7"));
}

// ----- ANTS-1888 — thinkingLevelFromLatestUserTurn() ---------------------

namespace {

// Append a {type:"user"} JSONL line with the given text content. The chip
// helper reads the LATEST user turn, so call order matters.
void appendUserTurn(QTemporaryFile &f, const QString &text) {
    if (!f.isOpen()) {
        // Re-open for append between writes — QTemporaryFile auto-removes.
        ASSERT_TRUE(f.open());
    }
    QJsonObject turn;
    turn["type"] = "user";
    QJsonObject msg;
    QJsonArray content;
    QJsonObject block;
    block["type"] = "text";
    block["text"] = text;
    content.append(block);
    msg["content"] = content;
    turn["message"] = msg;
    QTextStream out(&f);
    out << QJsonDocument(turn).toJson(QJsonDocument::Compact) << "\n";
    out.flush();
}

}  // namespace

TEST(ModelRecommenderThinkingLevel, AbsentTranscriptReturnsUnknown) {
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(
                  QStringLiteral("/no/such/file.jsonl")),
              ModelRecommender::ThinkingLevel::Unknown);
}

TEST(ModelRecommenderThinkingLevel, EmptyTranscriptReturnsUnknown) {
    QTemporaryFile f; ASSERT_TRUE(f.open()); f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Unknown);
}

TEST(ModelRecommenderThinkingLevel, PlainPromptIsStandard) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("Please fix the off-by-one in foo.cpp."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Standard);
}

TEST(ModelRecommenderThinkingLevel, UltrathinkDetected) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("ultrathink about whether to refactor this."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Ultrathink);
}

TEST(ModelRecommenderThinkingLevel, SlashUltrathinkDetected) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("/ultrathink"));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Ultrathink);
}

TEST(ModelRecommenderThinkingLevel, ThinkHardBeatsThink) {
    // Longest-match-wins: "think hard" must not match the bare "think" rule.
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("Please think hard about the cache."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::ThinkHard);
}

TEST(ModelRecommenderThinkingLevel, ThinkHarderAliasedToThinkHard) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("think harder than last time"));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::ThinkHard);
}

TEST(ModelRecommenderThinkingLevel, BareThinkDetected) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("think about this for a moment."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Think);
}

TEST(ModelRecommenderThinkingLevel, NoThinkExplicitlyStandard) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("/nothink — just do the bump."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Standard);
}

TEST(ModelRecommenderThinkingLevel, NoFalseMatchOnSubstring) {
    // "rethink" must not match "think"; "thinkpad" must not match "think".
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("Let's rethink the thinkpad design."));
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Standard);
}

TEST(ModelRecommenderThinkingLevel, LatestUserTurnWins) {
    QTemporaryFile f; ASSERT_TRUE(f.open());
    appendUserTurn(f, QStringLiteral("ultrathink the design"));
    appendUserTurn(f, QStringLiteral("just commit and push"));   // latest
    f.close();
    EXPECT_EQ(ModelRecommender::thinkingLevelFromLatestUserTurn(f.fileName()),
              ModelRecommender::ThinkingLevel::Standard);
}

// ----- ANTS-1890 — hasCommitIntent + weightForTurnIndex pure helpers -----

// INV-1 — exact verbs return true (caller pre-lowercases).
TEST(ModelRecommenderCommitIntent, CommitIntentExactWordsMatch) {
    using ModelRecommender::hasCommitIntent;
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("commit and push")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("git commit -m 'x'")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("please bump to 0.8.0")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("rebase onto main")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("stage these files")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("/commit")));
}

// INV-1 — `-ed`, `-ing`, `-s` inflected forms of every listed verb match.
TEST(ModelRecommenderCommitIntent, CommitIntentInflectedFormsMatch) {
    using ModelRecommender::hasCommitIntent;
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("i just committed; now push it")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("pushing the change now")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("staged everything ready")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("bumping the version")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("rebased onto main")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("commits land cleanly")));
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("pushes go through")));
}

// INV-1 — substrings inside larger words do NOT match.
TEST(ModelRecommenderCommitIntent, CommitIntentSubstringRejected) {
    using ModelRecommender::hasCommitIntent;
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("the committee met yesterday")));
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("we have a pushcart problem")));
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("on the thinkpad keyboard")));
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("plan the next sprint")));
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("")));
    // "bumper" must not match "bump" (substring rejection).
    EXPECT_FALSE(hasCommitIntent(QStringLiteral("the front bumper is dented")));
    // "rebases" — actually IS an inflection of rebase, should match.
    EXPECT_TRUE(hasCommitIntent(QStringLiteral("rebases the branch")));
}

// INV-3 — w(idx,total) is monotone non-decreasing in idx.
TEST(ModelRecommenderWeights, RecencyWeightingMonotone) {
    using ModelRecommender::weightForTurnIndex;
    const int total = 20;
    double prev = weightForTurnIndex(0, total);
    for (int i = 1; i < total; ++i) {
        const double curr = weightForTurnIndex(i, total);
        EXPECT_GE(curr, prev) << "idx=" << i << " curr=" << curr << " prev=" << prev;
        prev = curr;
    }
}

// INV-3 — endpoints: w(0,n)==1.0 for n>=1; w(total-1,total)==3.0 for total>=2.
TEST(ModelRecommenderWeights, RecencyWeightingEndpoints) {
    using ModelRecommender::weightForTurnIndex;
    for (int n : {1, 2, 5, 20, 100}) {
        EXPECT_DOUBLE_EQ(weightForTurnIndex(0, n), 1.0) << "n=" << n;
    }
    for (int n : {2, 5, 20, 100}) {
        EXPECT_DOUBLE_EQ(weightForTurnIndex(n - 1, n), 3.0) << "n=" << n;
    }
}

// INV-3 — singleton window: w(0,1)==1.0 (boundary).
TEST(ModelRecommenderWeights, RecencyWeightingSingleton) {
    using ModelRecommender::weightForTurnIndex;
    EXPECT_DOUBLE_EQ(weightForTurnIndex(0, 1), 1.0);
}

// INV-2 — commit-intent hard override beats a fully-positive
// refactoring window. 20 assistant turns each carrying one `Edit`
// tool_use AND one `text` block with a plan-keyword ("refactor") yields
// `fileWriteCount=20 (many_writes +2) + planKeyword=true (+2)` = sc=+4
// → Tier::Opus pre-fix. The trailing user turn "commit and push" must
// force Tier::Haiku post-fix, with reason "commit_intent".
TEST(ModelRecommenderCommitIntent, CommitIntentBeatsPriorRefactoringWindow) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 20; ++i) {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("refactor the cache")));
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    const QString path = writeSyntheticTranscriptWithUserTail(
        turns, QStringLiteral("commit and push"), f);
    const auto result = ModelRecommender::score(path);
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku)
        << "commit-intent override must beat sc=+4 ladder. reason: "
        << result.reason.toStdString();
    EXPECT_EQ(result.reason, QStringLiteral("commit_intent"));
}

// INV-2 — commit-intent fires even on a fresh project where the
// assistant-turn window is empty (the user typed "commit and push"
// before Claude responded). Function-body ordering: walk first, then
// override-branch BEFORE the empty-window short-circuit.
TEST(ModelRecommenderCommitIntent, CommitIntentFiresOnFreshSession) {
    QTemporaryFile f;
    const QString path = writeSyntheticTranscriptWithUserTail(
        {}, QStringLiteral("commit and push"), f);
    const auto result = ModelRecommender::score(path);
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku);
    EXPECT_EQ(result.reason, QStringLiteral("commit_intent"));
}

// INV-4 — recency-weighted writes: 4 writes in the LATEST 4 turns of a
// 20-turn window trip many_writes (weighted sum ≈ 11.368 ≥ 8). All other
// turns are mechanical (Bash) so plan_keyword + tool_diversity stay
// pinned low. Result must include "many_writes" in the reason.
TEST(ModelRecommenderRecency, WeightedWritesRecentClusterTrips) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    // Turns 0..15: mechanical Bash (no writes, no plan keyword)
    for (int i = 0; i < 16; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    // Turns 16..19: one Edit each (recent cluster — high weight)
    for (int i = 0; i < 4; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_TRUE(result.reason.contains(QStringLiteral("many_writes")))
        << "Recent-cluster weighted writes should trip many_writes. "
           "Got reason: " << result.reason.toStdString();
}

// INV-4 — 4 writes clustered in the OLDEST 4 turns of a 20-turn window
// do NOT trip many_writes (weighted sum ≈ 4.632 < 8). Reason must not
// contain "many_writes". (Pre-fix INV-4 would have tripped at >=4 writes
// regardless of position — RED.)
TEST(ModelRecommenderRecency, WeightedWritesOldClusterPasses) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    // Turns 0..3: one Edit each (oldest — low weight)
    for (int i = 0; i < 4; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    // Turns 4..19: mechanical Bash
    for (int i = 0; i < 16; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_FALSE(result.reason.contains(QStringLiteral("many_writes")))
        << "Old-cluster weighted writes should NOT trip many_writes "
           "(weighted sum ≈ 4.632 < 8). Got reason: "
        << result.reason.toStdString();
}

// INV-4 — toolDiversity stays UNWEIGHTED (set cardinality, position-
// independent). 6 distinct tool names anywhere in the window trips
// tool_diversity regardless of which turn each was used in.
TEST(ModelRecommenderRecency, ToolDiversityUnweighted) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    const QStringList tools = {
        QStringLiteral("Bash"),  QStringLiteral("Grep"),
        QStringLiteral("Glob"),  QStringLiteral("Read"),
        QStringLiteral("Edit"),  QStringLiteral("Write"),
    };
    // All 6 tools clustered in the OLDEST 6 turns; remaining 14 turns
    // are filler Bash. If weighting somehow leaked into diversity, the
    // old cluster wouldn't trip it; this asserts it still does.
    for (int i = 0; i < 6; ++i) {
        QJsonArray content;
        content.append(toolUse(tools[i]));
        turns.append(content);
    }
    for (int i = 0; i < 14; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_TRUE(result.reason.contains(QStringLiteral("tool_diversity")))
        << "toolDiversity must stay UNWEIGHTED (set cardinality across "
           "all turns). Got reason: " << result.reason.toStdString();
}

// INV-4 — planKeyword stays UNWEIGHTED (boolean OR across all turns).
// A single plan keyword in the OLDEST turn still trips plan_keyword.
TEST(ModelRecommenderRecency, PlanKeywordUnweighted) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    // Turn 0 (oldest): contains "refactor" plan keyword
    {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("Let's refactor the cache.")));
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    // Turns 1..19: filler Bash, no text
    for (int i = 0; i < 19; ++i) {
        QJsonArray content;
        content.append(toolUse(QStringLiteral("Bash")));
        turns.append(content);
    }
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript(turns, f));
    EXPECT_TRUE(result.reason.contains(QStringLiteral("plan_keyword")))
        << "planKeyword must stay UNWEIGHTED (OR across all turns). "
           "Got reason: " << result.reason.toStdString();
}

// INV-5 — single tail-read inside Result score(...). The body must
// contain exactly one `f.open(QIODevice::` occurrence — no
// thinkingLevel-style second tail-read.
TEST(ModelRecommenderSourceGrep, Inv5SingleFileOpenInScoreBody) {
    const std::string src = slurpFile(SRC_MODELRECOMMENDER_CPP_PATH);
    const std::string body = functionBody(src, "Result score(const QString");
    ASSERT_FALSE(body.empty()) << "could not slice score() body";
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find("f.open(QIODevice::", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    EXPECT_EQ(count, 1u)
        << "score() must perform exactly one tail-read; found "
        << count << " file-open sites in its body";
}

// INV-9 — purity signatures. Helpers take const QString & / primitives,
// return primitives, no Q_OBJECT / no slot machinery.
TEST(ModelRecommenderSourceGrep, Inv9PureHelperSignatures) {
    const std::string hdr = slurpFile(SRC_MODELRECOMMENDER_H_PATH);
    EXPECT_NE(hdr.find("bool hasCommitIntent(const QString &"), std::string::npos)
        << "hasCommitIntent must take const QString &";
    EXPECT_NE(hdr.find("double weightForTurnIndex(int idx, int total)"),
              std::string::npos)
        << "weightForTurnIndex must take (int, int) → double";
}

// INV-9 — neither helper performs I/O. Source-grep the implementations
// for any QFile / QIODevice access between their opener and closer.
TEST(ModelRecommenderSourceGrep, Inv9HelpersDoNoIO) {
    const std::string src = slurpFile(SRC_MODELRECOMMENDER_CPP_PATH);
    const std::string commitBody = functionBody(
        src, "bool hasCommitIntent(const QString &latestUserText)");
    const std::string weightBody = functionBody(
        src, "double weightForTurnIndex(int idx, int total)");
    ASSERT_FALSE(commitBody.empty());
    ASSERT_FALSE(weightBody.empty());
    EXPECT_EQ(commitBody.find("QFile"), std::string::npos)
        << "hasCommitIntent must not touch QFile";
    EXPECT_EQ(commitBody.find("QIODevice"), std::string::npos)
        << "hasCommitIntent must not touch QIODevice";
    EXPECT_EQ(weightBody.find("QFile"), std::string::npos)
        << "weightForTurnIndex must not touch QFile";
}

// Non-commit user turn must fall through to the additive ladder
// unchanged (regression safeguard: the override branch must not eat
// non-matching user turns).
TEST(ModelRecommenderCommitIntent, NonCommitUserTurnFallsThroughToLadder) {
    QTemporaryFile f;
    QVector<QJsonArray> turns;
    for (int i = 0; i < 20; ++i) {
        QJsonArray content;
        content.append(textBlock(QStringLiteral("refactor the cache")));
        content.append(toolUse(QStringLiteral("Edit")));
        turns.append(content);
    }
    const QString path = writeSyntheticTranscriptWithUserTail(
        turns, QStringLiteral("explain the architecture"), f);
    const auto result = ModelRecommender::score(path);
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Opus)
        << "non-commit user turn must NOT short-circuit; ladder should run. "
           "reason: " << result.reason.toStdString();
}

TEST(ModelRecommenderThinkingLevel, LabelMappings) {
    EXPECT_EQ(ModelRecommender::thinkingLevelLabel(
                  ModelRecommender::ThinkingLevel::Ultrathink),
              QStringLiteral("ultrathink"));
    EXPECT_EQ(ModelRecommender::thinkingLevelLabel(
                  ModelRecommender::ThinkingLevel::ThinkHard),
              QStringLiteral("think hard"));
    EXPECT_EQ(ModelRecommender::thinkingLevelLabel(
                  ModelRecommender::ThinkingLevel::Think),
              QStringLiteral("think"));
    EXPECT_EQ(ModelRecommender::thinkingLevelLabel(
                  ModelRecommender::ThinkingLevel::Standard),
              QStringLiteral("standard"));
    // Unknown maps to empty so the chip hides the thinking half rather
    // than ever rendering the word "Unknown".
    EXPECT_TRUE(ModelRecommender::thinkingLevelLabel(
                    ModelRecommender::ThinkingLevel::Unknown).isEmpty());
}

// ===== ANTS-1944 — score() provenance fields =====

// INV-1 — command path sets fromCommand=true; currentModelTsMs is don't-care
// (verified zero here because the writeTranscriptWithModelCommand helper omits
// timestamp on the assistant line — that's fine; INV-3 short-circuits on
// fromCommand before reading tsMs).
TEST(ModelRecommender, Ants1944CommandSetsFromCommand) {
    QTemporaryFile f;
    const auto result = ModelRecommender::score(writeTranscriptWithModelCommand(
        f, QStringLiteral("claude-sonnet-4-6"), QStringLiteral("haiku")));
    EXPECT_TRUE(result.currentModelFromCommand);
    // Existing command-wins semantics preserved.
    EXPECT_EQ(ModelRecommender::tierFromModelId(result.currentModel),
              ModelRecommender::Tier::Haiku);
}

// INV-2 — assistant-turn path sets fromCommand=false and carries the turn's
// parsed timestamp in currentModelTsMs. Guards the def→r propagation: if the
// fresh Result at modelrecommender.cpp:337-339 doesn't copy the fields, this
// test fails (fields stay at zero/false defaults).
TEST(ModelRecommender, Ants1944AssistantTurnSetsTimestamp) {
    // writeSyntheticTranscript stamps every turn with "2026-05-21T00:00:00Z";
    // QDateTime::fromString("2026-05-21T00:00:00.000Z", Qt::ISODate) → valid ms.
    QTemporaryFile f;
    const auto result = ModelRecommender::score(
        writeSyntheticTranscript({QJsonArray{}}, f, QStringLiteral("claude-opus-4-8")));
    EXPECT_FALSE(result.currentModelFromCommand);
    EXPECT_GT(result.currentModelTsMs, 0)
        << "expected a positive epoch-ms from the timestamp field";
    EXPECT_EQ(ModelRecommender::tierFromModelId(result.currentModel),
              ModelRecommender::Tier::Opus);
}
