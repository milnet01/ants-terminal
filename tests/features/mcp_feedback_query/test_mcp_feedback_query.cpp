// Feature-conformance test for the feedback_query MCP tool (ANTS-1961).
// Behavioural invariants drive the pure FeedbackFile::parse helper;
// refusal/envelope invariants drive cmdFeedbackQuery with an absolute
// path (m_main-independent). See spec.md + docs/specs/ANTS-1961.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// UTF-8 literals for the maintainer-anchor emoji.
const char *kClip = "\xF0\x9F\x93\x8B";  // 📋

QString writeFeedback(const QTemporaryDir &dir, const QString &name,
                      const QString &body) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(body.toUtf8());
    f.close();
    return path;
}

QStringList idsOf(const QJsonObject &env) {
    QStringList out;
    for (const auto &v : env.value("mapped_ids").toArray())
        out << v.toString();
    return out;
}

}  // namespace

// T1 — multi maintainer block, mixed heading forms; delta starts after
// the LAST (max-position) maintainer block.
TEST(McpFeedbackQuery, DeltaAfterLastMaintainer) {
    const QString body = QStringLiteral(
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# Ants MCP Feedback — Test\n\n"
        "## 2026-05-01 — first session\n\n"
        "### Old finding\n\n"
        "- **What:** stale.\n\n"
        "## %1 Ants Terminal roadmap tracking (added 2026-05-02)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| Old finding | ANTS-1111 | 📋 |\n\n"
        "## 2026-05-10 — second session\n\n"
        "### Mid finding\n\n"
        "- **What:** mid.\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| Mid finding | ANTS-2222 | \xE2\x9C\x85 |\n\n"
        "## 2026-05-20 — newest session\n\n"
        "### New finding\n\n"
        "- **What:** brand new.\n").arg(QString::fromUtf8(kClip));

    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    EXPECT_EQ(r.maintainerBlockCount, 2);
    EXPECT_TRUE(r.deltaPresent);
    EXPECT_TRUE(r.delta.contains("New finding"));
    EXPECT_FALSE(r.delta.contains("Mid finding"));
    EXPECT_FALSE(r.delta.contains("Old finding"));
    EXPECT_TRUE(r.delta.startsWith("## 2026-05-20"));
    EXPECT_GT(r.deltaStartLine, 0);
    EXPECT_GT(r.deltaLineCount, 0);
    // mapped_ids from maintainer bodies only, sorted + unique.
    QStringList ids = r.mappedIds;
    ASSERT_EQ(ids.size(), 2);
    EXPECT_EQ(ids.at(0), "ANTS-1111");
    EXPECT_EQ(ids.at(1), "ANTS-2222");
}

// T2 — a fenced `## Active` below the last maintainer block is not a
// boundary; the delta is not split by it.
TEST(McpFeedbackQuery, FencedHeadingNotBoundary) {
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| x | ANTS-3000 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n"
        "Here is a pasted roadmap snippet:\n\n"
        "```\n"
        "## Active\n"
        "- some item\n"
        "```\n\n"
        "Final prose line.\n").arg(QString::fromUtf8(kClip));

    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    EXPECT_EQ(r.maintainerBlockCount, 1);
    EXPECT_TRUE(r.deltaPresent);
    // The delta must contain BOTH the fenced "## Active" and the final
    // prose — proving the fence did not split it.
    EXPECT_TRUE(r.delta.contains("## Active"));
    EXPECT_TRUE(r.delta.contains("Final prose line."));
    EXPECT_TRUE(r.delta.startsWith("## 2026-05-20"));
}

// T3 — zero maintainer blocks: delta = everything after the H1 title;
// mapped_ids empty.
TEST(McpFeedbackQuery, ZeroMaintainerBlocks) {
    const QString body = QStringLiteral(
        "# Title line\n\n"
        "## 2026-05-20 — session\n\n"
        "### finding\n\n"
        "- **What:** something, cites ANTS-9999 in prose.\n");
    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    EXPECT_EQ(r.maintainerBlockCount, 0);
    EXPECT_EQ(r.lastMaintainerLine, -1);
    EXPECT_TRUE(r.deltaPresent);
    EXPECT_TRUE(r.delta.startsWith("## 2026-05-20"));
    EXPECT_FALSE(r.delta.contains("# Title line"));
    EXPECT_TRUE(r.mappedIds.isEmpty());  // contributor-cited ID excluded
}

// T4 — fully triaged: no contributor heading after the last maintainer
// block → empty delta, delta_present false, mapped_ids populated.
TEST(McpFeedbackQuery, FullyTriaged) {
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## 2026-05-10 — session\n\n"
        "### finding\n\n"
        "- **What:** triaged.\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| finding | ANTS-4444 | 📋 |\n\n"
        "End of 2026-05-11 maintainer roadmap-tracking update.\n")
        .arg(QString::fromUtf8(kClip));
    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    EXPECT_EQ(r.maintainerBlockCount, 1);
    EXPECT_FALSE(r.deltaPresent);
    EXPECT_EQ(r.delta, QString());
    EXPECT_EQ(r.deltaStartLine, -1);
    EXPECT_EQ(r.deltaLineCount, 0);
    ASSERT_EQ(r.mappedIds.size(), 1);
    EXPECT_EQ(r.mappedIds.at(0), "ANTS-4444");
}

// T5 — mapped-id scoping: contributor-cited ID below the watermark is
// excluded; one in a maintainer table is included.
TEST(McpFeedbackQuery, MappedIdScoping) {
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| a | ANTS-5000 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n"
        "This references ANTS-6000 in contributor prose.\n")
        .arg(QString::fromUtf8(kClip));
    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    ASSERT_EQ(r.mappedIds.size(), 1);
    EXPECT_EQ(r.mappedIds.at(0), "ANTS-5000");
    EXPECT_TRUE(r.delta.contains("ANTS-6000"));  // present in delta text
}

// T6 — ### headings inside blocks are inert (never split a block).
TEST(McpFeedbackQuery, DeeperHeadingsInert) {
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "### sub-note inside maintainer block\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| a | ANTS-7000 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n"
        "### finding one\n\n"
        "- **What:** a.\n\n"
        "### finding two\n\n"
        "- **What:** b.\n").arg(QString::fromUtf8(kClip));
    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    EXPECT_EQ(r.maintainerBlockCount, 1);
    // The maintainer block's ### sub-note did not start the delta.
    EXPECT_TRUE(r.delta.startsWith("## 2026-05-20"));
    // Both ### findings stayed in one contributor delta.
    EXPECT_TRUE(r.delta.contains("finding one"));
    EXPECT_TRUE(r.delta.contains("finding two"));
    ASSERT_EQ(r.mappedIds.size(), 1);
    EXPECT_EQ(r.mappedIds.at(0), "ANTS-7000");
}

// T1 (handler) — envelope carries all INV-8 fields.
TEST(McpFeedbackQuery, HandlerEnvelopeFields) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| a | ANTS-8000 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n"
        "### finding\n\n- **What:** x.\n").arg(QString::fromUtf8(kClip));
    const QString p = writeFeedback(dir, "X_Ants_MCP_Feedback.md", body);
    ASSERT_FALSE(p.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool()) << "expected ok envelope";
    EXPECT_TRUE(env.contains("delta"));
    EXPECT_TRUE(env.value("delta_present").toBool());
    EXPECT_GT(env.value("delta_line_count").toInt(), 0);
    EXPECT_GT(env.value("delta_start_line").toInt(), 0);
    EXPECT_EQ(env.value("maintainer_block_count").toInt(), 1);
    EXPECT_GT(env.value("last_maintainer_line").toInt(), 0);
    EXPECT_FALSE(env.value("truncated").toBool());
    EXPECT_EQ(idsOf(env), QStringList{"ANTS-8000"});
    EXPECT_EQ(env.value("path").toString(), p);
}

// T7 — refusals.
TEST(McpFeedbackQuery, Refusals) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);

    // missing path → bad_args
    { QJsonObject req; req["caller_cwd"] = dir.path();
      const QJsonObject e = rc.cmdFeedbackQuery(req).object();
      EXPECT_FALSE(e.value("ok").toBool());
      EXPECT_EQ(e.value("code").toString(), "bad_args"); }

    // non-feedback basename → not_feedback_file
    { const QString p = writeFeedback(dir, "notes.md", "# x\n");
      QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
      const QJsonObject e = rc.cmdFeedbackQuery(req).object();
      EXPECT_FALSE(e.value("ok").toBool());
      EXPECT_EQ(e.value("code").toString(), "not_feedback_file"); }

    // well-formed, non-existent feedback file → not_found
    { const QString p = dir.path() + "/Gone_Ants_MCP_Feedback.md";
      QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
      const QJsonObject e = rc.cmdFeedbackQuery(req).object();
      EXPECT_FALSE(e.value("ok").toBool());
      EXPECT_EQ(e.value("code").toString(), "not_found"); }
}

// ANTS-3366 — a not_found envelope lists sibling *_Ants_MCP_Feedback.md
// files in the same dir under `candidates` (+ a `hint`) so a caller that
// derived the wrong basename (e.g. the doubled-suffix "DOOM_Ants" case)
// recovers without shelling out to `ls`.
TEST(McpFeedbackQuery, NotFoundListsSiblingCandidates) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    RemoteControl rc(nullptr);

    // A real sibling exists; the query asks for a doubled-suffix path that
    // does not.
    const QString real =
        writeFeedback(dir, "DOOM_Ants_MCP_Feedback.md", "# x\n");
    ASSERT_FALSE(real.isEmpty());
    const QString wrong =
        dir.path() + "/DOOM_Ants_Ants_MCP_Feedback.md";  // doubled token

    QJsonObject req; req["path"] = wrong; req["caller_cwd"] = dir.path();
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    EXPECT_FALSE(e.value("ok").toBool());
    EXPECT_EQ(e.value("code").toString(), "not_found");
    ASSERT_TRUE(e.contains("candidates"));
    const QJsonArray cands = e.value("candidates").toArray();
    ASSERT_EQ(cands.size(), 1);
    EXPECT_EQ(cands.at(0).toString(), real);
    EXPECT_TRUE(e.contains("hint"));

    // No sibling → no candidates key (lean envelope unchanged).
    QTemporaryDir empty; ASSERT_TRUE(empty.isValid());
    QJsonObject req2;
    req2["path"] = empty.path() + "/Gone_Ants_MCP_Feedback.md";
    req2["caller_cwd"] = empty.path();
    const QJsonObject e2 = rc.cmdFeedbackQuery(req2).object();
    EXPECT_EQ(e2.value("code").toString(), "not_found");
    EXPECT_FALSE(e2.contains("candidates"));
}

// T8 — byte cap: head kept, truncated true, full line count reported.
TEST(McpFeedbackQuery, ByteCapKeepsHead) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| a | ANTS-1 | 📋 |\n\n"
        "## 2026-05-20 — session\n\n").arg(QString::fromUtf8(kClip));
    QString headMarker = QStringLiteral("- **What:** HEADMARKER line.\n");
    body += headMarker;
    for (int i = 0; i < 200; ++i)
        body += QStringLiteral("- filler line number %1 padding padding\n").arg(i);
    body += QStringLiteral("- **What:** TAILMARKER line.\n");
    const QString p = writeFeedback(dir, "Big_Ants_MCP_Feedback.md", body);

    RemoteControl rc(nullptr);
    QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
    req["max_bytes"] = 200;
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("truncated").toBool());
    const QString delta = env.value("delta").toString();
    EXPECT_TRUE(delta.contains("HEADMARKER"));     // head kept
    EXPECT_FALSE(delta.contains("TAILMARKER"));     // tail dropped
    EXPECT_GT(env.value("delta_line_count").toInt(), 100);  // full count
}

// ANTS-2226 — the skeleton's contributor banner names the read/write verbs
// so a contributor session discovers them from the file itself, and the
// banner (a blockquote above the H1's content) is inert to the
// boundary-heading delta parser once a maintainer block exists.
TEST(McpFeedbackQuery, SkeletonBannerAdvertisesVerbs) {
    const QString sk = FeedbackFile::skeleton(QStringLiteral("Demo Project"));
    EXPECT_TRUE(sk.contains(QStringLiteral("feedback_query")))
        << "skeleton must name the feedback_query verb";
    EXPECT_TRUE(sk.contains(QStringLiteral("feedback_log")))
        << "skeleton must name the feedback_log verb";
    // Inert to the parser: with a maintainer block present (the steady
    // state of a triaged file), the banner sits ABOVE it, so no contributor
    // heading follows the last maintainer block — zero un-triaged delta.
    const QString triaged = sk +
        QString::fromUtf8(
            "\n## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
            "(2026-06-27, maintainer)\n\n| a | ANTS-1 | shipped |\n");
    const FeedbackFile::ParseResult r = FeedbackFile::parse(triaged);
    EXPECT_EQ(r.maintainerBlockCount, 1);
    EXPECT_FALSE(r.deltaPresent)
        << "the blockquote banner above the maintainer block must not "
           "register as un-triaged delta";
}
