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
#include <QFileInfo>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// UTF-8 literals for the maintainer-anchor + status emoji. (A `\x..`
// escape inside QStringLiteral would be mis-read as separate UTF-16
// units, so status emoji go in source bodies as literal glyphs and these
// narrow-byte constants drive the comparisons via QString::fromUtf8.)
const char *kClip  = "\xF0\x9F\x93\x8B";  // 📋
const char *kCheck = "\xE2\x9C\x85";      // ✅

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

    // missing path AND no resolvable caller_cwd → bad_args (ANTS-3376:
    // with a resolvable caller_cwd the path is now derived instead).
    { QJsonObject req;
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

// ANTS-3376 — with `path` omitted, the conventional
// <caller_cwd-leaf>_Ants_MCP_Feedback.md at the shared root (the parent of
// caller_cwd) is derived; an existing file at that default reads back with
// path_derived:true.
TEST(McpFeedbackQuery, DerivesDefaultPathFromCallerCwd) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    // caller_cwd is a project dir under the shared root; the feedback file
    // lives in the shared root, named for the project leaf.
    ASSERT_TRUE(QDir(root.path()).mkdir("RetroDB"));
    const QString caller = root.path() + "/RetroDB";
    const QString derived =
        writeFeedback(root, "RetroDB_Ants_MCP_Feedback.md",
                      "<!-- ants-mcp-feedback: 1 -->\n# x\n\n"
                      "## 2026-06-30 — s\n\n- **What:** new.\n");
    ASSERT_FALSE(derived.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req; req["caller_cwd"] = caller;   // no "path"
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(e.value("ok").toBool());
    EXPECT_EQ(QFileInfo(e.value("path").toString()).fileName(),
              "RetroDB_Ants_MCP_Feedback.md");
    EXPECT_TRUE(e.value("path_derived").toBool());
    EXPECT_TRUE(e.value("delta").toString().contains("new."));

    // Derived default that does not exist → not_found, and because the
    // caller's own sibling is absent while OTHER projects' files exist, the
    // envelope flags all_other_projects.
    QTemporaryDir root2; ASSERT_TRUE(root2.isValid());
    ASSERT_TRUE(QDir(root2.path()).mkdir("BrandNew"));
    writeFeedback(root2, "Other_Ants_MCP_Feedback.md", "# o\n");
    QJsonObject req2; req2["caller_cwd"] = root2.path() + "/BrandNew";
    const QJsonObject e2 = rc.cmdFeedbackQuery(req2).object();
    EXPECT_EQ(e2.value("code").toString(), "not_found");
    EXPECT_TRUE(e2.value("all_other_projects").toBool());
    EXPECT_TRUE(e2.contains("candidates"));
}

// ANTS-3376 — a not_found candidate list floats the caller's OWN file to
// the front so the obvious retry is candidates[0].
TEST(McpFeedbackQuery, NotFoundFloatsOwnFileFirst) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("RetroDB"));
    // Two siblings present; the caller's own (RetroDB) is written second so
    // name-order would otherwise place "AAA" first.
    writeFeedback(dir, "AAA_Ants_MCP_Feedback.md", "# a\n");
    const QString own =
        writeFeedback(dir, "RetroDB_Ants_MCP_Feedback.md", "# r\n");

    RemoteControl rc(nullptr);
    // Ask for a wrong basename so it 404s but siblings are listed.
    QJsonObject req;
    req["path"] = dir.path() + "/Wrong_Ants_MCP_Feedback.md";
    req["caller_cwd"] = dir.path() + "/RetroDB";
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    EXPECT_EQ(e.value("code").toString(), "not_found");
    const QJsonArray cands = e.value("candidates").toArray();
    ASSERT_EQ(cands.size(), 2);
    EXPECT_EQ(cands.at(0).toString(), own);     // own file first
    EXPECT_FALSE(e.value("all_other_projects").toBool());
}

// ANTS-3439 — the checkout-dir leaf need not equal the feedback file's
// package-name stem (`Fin_Break` vs `finbreak`). With `path` omitted, a
// normalized-equal sibling (lowercase, strip `_`/`-`) is adopted as the
// derived default and reads back ok:true, path_derived:true.
TEST(McpFeedbackQuery, DerivesDefaultAcrossLeafPackageMismatch) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("Fin_Break"));
    const QString caller = root.path() + "/Fin_Break";
    // The file ships under the package name, not the checkout-dir leaf.
    const QString derived =
        writeFeedback(root, "finbreak_Ants_MCP_Feedback.md",
                      "<!-- ants-mcp-feedback: 1 -->\n# x\n\n"
                      "## 2026-07-04 — s\n\n- **What:** mismatch resolved.\n");
    ASSERT_FALSE(derived.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req; req["caller_cwd"] = caller;   // no "path"
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(e.value("ok").toBool());
    EXPECT_EQ(QFileInfo(e.value("path").toString()).fileName(),
              "finbreak_Ants_MCP_Feedback.md");
    EXPECT_TRUE(e.value("path_derived").toBool());
    EXPECT_TRUE(e.value("delta").toString().contains("mismatch resolved."));
}

// ANTS-3439 — a not_found envelope floats the normalized-equal sibling to
// candidates[0] and does NOT flag all_other_projects (the checkout-dir leaf
// `Fin_Break` and the file stem `finbreak` differ only by case/separators).
TEST(McpFeedbackQuery, NotFoundNormalizedOwnMatch) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("Fin_Break"));
    writeFeedback(dir, "AAA_Ants_MCP_Feedback.md", "# a\n");
    const QString own =
        writeFeedback(dir, "finbreak_Ants_MCP_Feedback.md", "# f\n");

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = dir.path() + "/Wrong_Ants_MCP_Feedback.md";  // 404s
    req["caller_cwd"] = dir.path() + "/Fin_Break";
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    EXPECT_EQ(e.value("code").toString(), "not_found");
    const QJsonArray cands = e.value("candidates").toArray();
    ASSERT_EQ(cands.size(), 2);
    EXPECT_EQ(cands.at(0).toString(), own);          // normalized own file first
    EXPECT_FALSE(e.value("all_other_projects").toBool());
    EXPECT_TRUE(e.value("hint").toString().contains("normalizes"));
}

// ANTS-3426 — a checkout leaf that already ends in "_Ants" (fork naming like
// "DOOM_Ants") would double the token when the suffix is appended
// ("DOOM_Ants_Ants_MCP_Feedback.md"), which never exists. With `path` omitted
// the derivation drops the redundant "_Ants" and adopts the real
// "DOOM_Ants_MCP_Feedback.md". Deterministic — the leaf normalizes to
// "doomants" while the file stem is "doom", so ANTS-3439's normalized scan
// cannot catch it; this is an exact second candidate, not a fuzzy accept.
TEST(McpFeedbackQuery, DerivesDefaultDeDoublesTrailingAnts) {
    QTemporaryDir root; ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("DOOM_Ants"));
    const QString caller = root.path() + "/DOOM_Ants";
    const QString real =
        writeFeedback(root, "DOOM_Ants_MCP_Feedback.md",
                      "<!-- ants-mcp-feedback: 1 -->\n# x\n\n"
                      "## 2026-07-09 — s\n\n- **What:** de-doubled.\n");
    ASSERT_FALSE(real.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject req; req["caller_cwd"] = caller;   // no "path"
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(e.value("ok").toBool());
    EXPECT_EQ(QFileInfo(e.value("path").toString()).fileName(),
              "DOOM_Ants_MCP_Feedback.md");        // NOT the doubled token
    EXPECT_TRUE(e.value("path_derived").toBool());
    EXPECT_TRUE(e.value("delta").toString().contains("de-doubled."));
}

// ANTS-3426 — a not_found envelope for an "_Ants"-suffixed leaf recognises the
// de-doubled sibling as the caller's OWN file (floated first, no
// all_other_projects, hint names the real de-doubled basename) rather than
// mislabelling it as another project's file.
TEST(McpFeedbackQuery, NotFoundDeDoubledOwnMatch) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("DOOM_Ants"));
    writeFeedback(dir, "AAA_Ants_MCP_Feedback.md", "# a\n");
    const QString own =
        writeFeedback(dir, "DOOM_Ants_MCP_Feedback.md", "# d\n");

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = dir.path() + "/Wrong_Ants_MCP_Feedback.md";  // 404s
    req["caller_cwd"] = dir.path() + "/DOOM_Ants";
    const QJsonObject e = rc.cmdFeedbackQuery(req).object();
    EXPECT_EQ(e.value("code").toString(), "not_found");
    const QJsonArray cands = e.value("candidates").toArray();
    ASSERT_EQ(cands.size(), 2);
    EXPECT_EQ(cands.at(0).toString(), own);          // de-doubled own file first
    EXPECT_FALSE(e.value("all_other_projects").toBool());
    EXPECT_TRUE(e.value("hint").toString().contains("DOOM_Ants_MCP_Feedback.md"));
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

// ANTS-3371 — parse() extracts every maintainer tracking-table data row
// (item, ids, status, notes), skipping the header + `|---|` separator,
// across all maintainer blocks in document order. A later block's row for
// the same item appears after the earlier one (caller sees the latest).
TEST(McpFeedbackQuery, TrackingRowsExtracted) {
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| First idea | ANTS-1000, ANTS-1001 | 📋 |\n"
        "| Vague idea | n/a | 💭 |\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-20, maintainer)\n\n"
        "| Item | ID(s) | Status | Notes |\n"
        "|------|-------|--------|-------|\n"
        "| First idea | ANTS-1000 | ✅ | Shipped 2026-05-20. |\n")
        .arg(QString::fromUtf8(kClip));
    const FeedbackFile::ParseResult r = FeedbackFile::parse(body);
    ASSERT_EQ(r.trackingRows.size(), 3);
    // Block 1 row 1 — two comma-split IDs, no notes column.
    EXPECT_EQ(r.trackingRows.at(0).item, "First idea");
    ASSERT_EQ(r.trackingRows.at(0).ids.size(), 2);
    EXPECT_EQ(r.trackingRows.at(0).ids.at(0), "ANTS-1000");
    EXPECT_EQ(r.trackingRows.at(0).ids.at(1), "ANTS-1001");
    EXPECT_EQ(r.trackingRows.at(0).status, QString::fromUtf8(kClip));  // 📋
    EXPECT_TRUE(r.trackingRows.at(0).notes.isEmpty());
    // Block 1 row 2 — "n/a" yields no IDs.
    EXPECT_EQ(r.trackingRows.at(1).item, "Vague idea");
    EXPECT_TRUE(r.trackingRows.at(1).ids.isEmpty());
    // Block 2 row — later in document order, ✅ + notes column.
    EXPECT_EQ(r.trackingRows.at(2).item, "First idea");
    EXPECT_EQ(r.trackingRows.at(2).status, QString::fromUtf8(kCheck));  // ✅
    EXPECT_EQ(r.trackingRows.at(2).notes, "Shipped 2026-05-20.");
}

// ANTS-3371 — include_tracking surfaces the rows in the envelope; absent
// flag keeps the lean envelope (no `tracking` key).
TEST(McpFeedbackQuery, IncludeTrackingEnvelope) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString body = QStringLiteral(
        "# Title\n\n"
        "## %1 Ants Terminal roadmap tracking update (2026-05-11, maintainer)\n\n"
        "| Item | ID(s) | Status |\n"
        "|------|-------|--------|\n"
        "| idea | ANTS-9001 | ✅ |\n").arg(QString::fromUtf8(kClip));
    const QString p = writeFeedback(dir, "Trk_Ants_MCP_Feedback.md", body);
    ASSERT_FALSE(p.isEmpty());
    RemoteControl rc(nullptr);

    // Without the flag: no `tracking` key (lean envelope unchanged).
    {
        QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
        const QJsonObject env = rc.cmdFeedbackQuery(req).object();
        ASSERT_TRUE(env.value("ok").toBool());
        EXPECT_FALSE(env.contains("tracking"));
    }
    // With the flag: a `tracking` array with the parsed row.
    {
        QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
        req["include_tracking"] = true;
        const QJsonObject env = rc.cmdFeedbackQuery(req).object();
        ASSERT_TRUE(env.value("ok").toBool());
        ASSERT_TRUE(env.contains("tracking"));
        const QJsonArray trk = env.value("tracking").toArray();
        ASSERT_EQ(trk.size(), 1);
        const QJsonObject row = trk.at(0).toObject();
        EXPECT_EQ(row.value("item").toString(), "idea");
        const QJsonArray ids = row.value("ids").toArray();
        ASSERT_EQ(ids.size(), 1);
        EXPECT_EQ(ids.at(0).toString(), "ANTS-9001");
        EXPECT_EQ(row.value("status").toString(), QString::fromUtf8(kCheck));
    }
}

// ANTS-3478 — feedback_query resolves each mapped id's LIVE status from the
// caller project's ROADMAP.md into `mapped_id_status` [{id, status}]: a
// triaged id renders 📋/🚧/✅, an id absent from the live roadmap renders
// "unknown" (never silently ✅). Present only when mapped_ids is non-empty.
TEST(McpFeedbackQuery, MappedIdStatusResolvesLiveRoadmap) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    // v2 file: three findings with FILLED Proposed IDs → three mapped_ids.
    const QString body = QStringLiteral(
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# Ants MCP Feedback — Test\n\n"
        "## 2026-07-10 — s\n\n"
        "### Finding A\n\n- **What:** a.\n- **Proposed ID:** ANTS-3100\n\n"
        "### Finding B\n\n- **What:** b.\n- **Proposed ID:** ANTS-3200\n\n"
        "### Finding C\n\n- **What:** c.\n- **Proposed ID:** ANTS-3999\n");
    const QString p = writeFeedback(dir, "Live_Ants_MCP_Feedback.md", body);
    ASSERT_FALSE(p.isEmpty());
    // ROADMAP.md in the caller project: ANTS-100 shipped, ANTS-200 planned,
    // ANTS-999 absent (never appears).
    QFile rm(dir.path() + "/ROADMAP.md");
    ASSERT_TRUE(rm.open(QIODevice::WriteOnly | QIODevice::Truncate));
    // Emoji as literal UTF-8 glyphs — a `\x..` escape inside QStringLiteral
    // would be mis-read as separate UTF-16 units (see the file header note).
    rm.write(QString::fromUtf8(
        "# ROADMAP\n\n"
        "- \xE2\x9C\x85 [ANTS-3100] **Shipped one.**\n"
        "- \xF0\x9F\x93\x8B [ANTS-3200] **Planned one.**\n").toUtf8());
    rm.close();

    RemoteControl rc(nullptr);
    QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_TRUE(env.contains("mapped_id_status"));

    QHash<QString, QString> got;
    for (const auto &v : env.value("mapped_id_status").toArray()) {
        const QJsonObject o = v.toObject();
        got.insert(o.value("id").toString(), o.value("status").toString());
    }
    EXPECT_EQ(got.value("ANTS-3100"), QString::fromUtf8(kCheck));   // ✅ live
    EXPECT_EQ(got.value("ANTS-3200"), QString::fromUtf8(kClip));    // 📋 live
    EXPECT_EQ(got.value("ANTS-3999"), QStringLiteral("unknown"));   // absent
}

// ANTS-3518 — a mapped id whose prefix is absent from the caller roadmap's own
// id-prefixes is cross-repo, not merely archived: resolve it to "foreign_repo"
// (with a top-level mapped_id_status_note) instead of the ambiguous "unknown",
// so a consumer session doesn't misread a shipped cross-repo suggestion as
// never-shipped. A same-prefix id that is simply absent stays "unknown".
TEST(McpFeedbackQuery, MappedIdStatusForeignRepo) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    // v2 file: mapped ids are always ANTS-* (the parser's id regex). From a
    // consumer project whose roadmap uses a different prefix, those ids are
    // cross-repo — the exact case that made every ANTS id read "unknown".
    const QString body = QStringLiteral(
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# Ants MCP Feedback — Test\n\n"
        "## 2026-07-16 — s\n\n"
        "### Finding A\n\n- **What:** a.\n- **Proposed ID:** ANTS-3517\n\n"
        "### Finding B\n\n- **What:** b.\n- **Proposed ID:** ANTS-3518\n");
    const QString p = writeFeedback(dir, "Fibr_Ants_MCP_Feedback.md", body);
    ASSERT_FALSE(p.isEmpty());
    // The consumer project's ROADMAP uses the FIBR prefix and contains no ANTS
    // ids at all — so the ANTS mapped ids belong to a foreign roadmap.
    QFile rm(dir.path() + "/ROADMAP.md");
    ASSERT_TRUE(rm.open(QIODevice::WriteOnly | QIODevice::Truncate));
    rm.write(QString::fromUtf8(
        "# ROADMAP\n\n"
        "- \xF0\x9F\x93\x8B [FIBR-0100] **A planned local item.**\n").toUtf8());
    rm.close();

    RemoteControl rc(nullptr);
    QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_TRUE(env.contains("mapped_id_status"));

    QHash<QString, QString> got;
    for (const auto &v : env.value("mapped_id_status").toArray()) {
        const QJsonObject o = v.toObject();
        got.insert(o.value("id").toString(), o.value("status").toString());
    }
    // ANTS prefix absent from the FIBR caller roadmap → cross-repo, not the
    // ambiguous "unknown" (which a consumer would misread as "never shipped").
    EXPECT_EQ(got.value("ANTS-3517"), QStringLiteral("foreign_repo"));
    EXPECT_EQ(got.value("ANTS-3518"), QStringLiteral("foreign_repo"));
    // The note fires whenever any mapped id is foreign_repo.
    EXPECT_TRUE(env.contains("mapped_id_status_note"));
    // Sibling test MappedIdStatusResolvesLiveRoadmap covers the same-prefix
    // fallback: an ANTS id absent from an ANTS-prefixed roadmap stays "unknown".
}

// ANTS-3478 — a fresh / untriaged file (no filled Proposed IDs → empty
// mapped_ids) omits mapped_id_status entirely (no roadmap read cost).
TEST(McpFeedbackQuery, MappedIdStatusAbsentWhenNoMappedIds) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString body = QStringLiteral(
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# Ants MCP Feedback — Test\n\n"
        "## 2026-07-10 — s\n\n"
        "### Untriaged\n\n- **What:** x.\n"
        "- **Proposed ID:** _(maintainer to assign)_\n");
    const QString p = writeFeedback(dir, "Fresh_Ants_MCP_Feedback.md", body);
    ASSERT_FALSE(p.isEmpty());
    RemoteControl rc(nullptr);
    QJsonObject req; req["path"] = p; req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("mapped_ids").toArray().isEmpty());
    EXPECT_FALSE(env.contains("mapped_id_status"));
}
