// Feature-conformance test for ANTS-1226 — ModelRecommender scorer.
// See tests/features/model_recommender/spec.md.

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QTextStream>
#include "modelrecommender.h"

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

// INV-2: plan keyword + 4 writes → Opus
TEST(ModelRecommender, Inv2PlanKeywordPlusFourWritesReturnsOpus) {
    QTemporaryFile f;
    QJsonArray content;
    content.append(textBlock(QStringLiteral("Let me design the architecture for this spec.")));
    content.append(toolUse(QStringLiteral("Edit")));
    content.append(toolUse(QStringLiteral("Edit")));
    content.append(toolUse(QStringLiteral("Edit")));
    content.append(toolUse(QStringLiteral("Edit")));
    QVector<QJsonArray> turns;
    turns.append(content);
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
    // With 20 mechanical turns: score ≤ -1 → Haiku
    EXPECT_EQ(result.tier, ModelRecommender::Tier::Haiku)
        << "Only the last 20 turns should be scored. "
           "reason: " << result.reason.toStdString();
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
