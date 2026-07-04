// ANTS-3442 — feature-conformance test for feedback_log op:"prune_tracking".
// Pure FeedbackFile::pruneTracking over a synthetic fixture + a live
// RemoteControl::cmdFeedbackLog drive + schema/dispatch source-greps.
// See spec.md + docs/specs/ANTS-3442.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

// 📋=\xF0\x9F\x93\x8B  🚧=\xF0\x9F\x9A\xA7  ✅=\xE2\x9C\x85
const char *kFixture =
    "<!-- ants-mcp-feedback: 1 -->\n"
    "# Ants MCP Feedback TEST\n"
    "\n"
    "> Contributors append below the last maintainer block.\n"
    "\n"
    "## 2026-06-01 \xE2\x80\x94 s1\n"
    "\n"
    "### F1\n"
    "\n"
    "Body.\n"
    "\n"
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
    "(2026-06-01, maintainer)\n"
    "\n"
    "| Item | IDs | Status |\n"
    "|---|---|---|\n"
    "| First look | ANTS-0001 | \xF0\x9F\x93\x8B |\n"
    "| Batch A + B | ANTS-0002, ANTS-0003 | \xF0\x9F\x93\x8B |\n"
    "| Look7 | ANTS-0007 | \xF0\x9F\x93\x8B |\n"
    "\n"
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
    "(2026-06-15, maintainer)\n"
    "\n"
    "| Item | IDs | Status | Notes |\n"
    "|---|---|---|---|\n"
    "| Started | ANTS-0001 | \xF0\x9F\x9A\xA7 |  |\n"
    "| B done | ANTS-0003 | \xE2\x9C\x85 |  |\n"
    "| Prose note | n/a | \xE2\x9C\x85 | keep me |\n"
    "| R1 | ANTS-0005 | \xF0\x9F\x93\x8B | see ANTS-0009 |\n"
    "| R2 | ANTS-0006 | \xF0\x9F\x93\x8B | see ANTS-0009 |\n"
    "\n"
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update "
    "(2026-07-01, maintainer)\n"
    "\n"
    "| Item | IDs | Status |\n"
    "|---|---|---|\n"
    "| Shipped | ANTS-0001 | \xE2\x9C\x85 |\n"
    "| Ship7 | ANTS-0007 | \xE2\x9C\x85 |\n"
    "| R1 done | ANTS-0005 | \xE2\x9C\x85 |\n"
    "| R2 done | ANTS-0006 | \xE2\x9C\x85 |\n";

QString fx() { return QString::fromUtf8(kFixture); }

int countHeadings(const QString &content) {
    int n = 0;
    for (const QString &l : content.split(QLatin1Char('\n')))
        if (l.contains(QStringLiteral(", maintainer)"))) ++n;
    return n;
}

bool writeStr(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray u = body.toUtf8();
    const bool ok = (f.write(u) == u.size());
    f.close();
    return ok;
}
QString readStr(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}
QString seed(QTemporaryDir &dir) {
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    writeStr(p, fx());
    return p;
}

std::string slurp(const char *path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// INV-2/7 — fully-superseded single-id rows removed; last rows survive.
TEST(FeedbackPruneTracking, Inv2SupersededRemoved) {
    const FeedbackFile::PruneResult r =
        FeedbackFile::pruneTracking(fx(), {});
    ASSERT_EQ(r.removed.size(), 3);   // First look, Look7, Started
    EXPECT_EQ(r.removed.at(0).ids, QStringList{QStringLiteral("ANTS-0001")});
    EXPECT_EQ(r.removed.at(1).ids, QStringList{QStringLiteral("ANTS-0007")});
    EXPECT_EQ(r.removed.at(2).ids, QStringList{QStringLiteral("ANTS-0001")});
    EXPECT_GT(r.bytesSaved, 0);
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("First look")));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("| Look7 |")));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("| Started |")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| Shipped |")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| Ship7 |")));
}

// INV-3 — a multi-id row survives if it is last-of-any-id (ANTS-0002).
TEST(FeedbackPruneTracking, Inv3MultiIdLastOfOneSurvives) {
    const FeedbackFile::PruneResult r =
        FeedbackFile::pruneTracking(fx(), {});
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| Batch A + B |")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| B done |")));
}

// INV-4 — an n/a row is never removed.
TEST(FeedbackPruneTracking, Inv4NaRowKept) {
    const FeedbackFile::PruneResult r =
        FeedbackFile::pruneTracking(fx(), {});
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| Prose note |")));
}

// INV-2b — two marked rows whose only ANTS-0009 citation is their notes cell
// are BOTH pinned, so ANTS-0009 is not dropped.
TEST(FeedbackPruneTracking, Inv2bNotesCellPinPreservesMappedId) {
    const FeedbackFile::PruneResult r =
        FeedbackFile::pruneTracking(fx(), {});
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("ANTS-0009")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| R1 |")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| R2 |")));
    // parse().mappedIds is preserved.
    const QStringList before = FeedbackFile::parse(fx()).mappedIds;
    const QStringList after = FeedbackFile::parse(r.newContent).mappedIds;
    EXPECT_EQ(before, after);
}

// INV-5 — every maintainer heading survives.
TEST(FeedbackPruneTracking, Inv5HeadingsKept) {
    const FeedbackFile::PruneResult r =
        FeedbackFile::pruneTracking(fx(), {});
    EXPECT_EQ(countHeadings(fx()), 3);
    EXPECT_EQ(countHeadings(r.newContent), 3);
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("|---|---|---|")));
}

// INV-6 — scope_ids restricts to the named ids (ID column only).
TEST(FeedbackPruneTracking, Inv6ScopeIds) {
    FeedbackFile::PruneOptions opts;
    opts.scopeIds << QStringLiteral("ANTS-0001");
    const FeedbackFile::PruneResult r = FeedbackFile::pruneTracking(fx(), opts);
    ASSERT_EQ(r.removed.size(), 2);   // First look, Started — NOT Look7
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| Look7 |")));
}

// INV-9 — idempotent: a second prune removes nothing.
TEST(FeedbackPruneTracking, Inv9Idempotent) {
    const FeedbackFile::PruneResult r1 =
        FeedbackFile::pruneTracking(fx(), {});
    const FeedbackFile::PruneResult r2 =
        FeedbackFile::pruneTracking(r1.newContent, {});
    EXPECT_EQ(r2.removed.size(), 0);
    EXPECT_EQ(r2.bytesSaved, 0);
    EXPECT_EQ(r2.newContent, r1.newContent);
}

// Live wrapper — dry_run previews without writing.
TEST(FeedbackPruneTracking, LiveDryRunNoWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const QString before = readStr(p);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "prune_tracking";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["dry_run"] = true;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("rows_removed").toInt(), 3);
    EXPECT_GT(env.value("bytes_saved").toInt(), 0);
    EXPECT_TRUE(env.value("dry_run").toBool());
    EXPECT_EQ(readStr(p), before);   // file untouched
}

// Live wrapper — real prune writes; envelope shape.
TEST(FeedbackPruneTracking, LiveRealWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "prune_tracking";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("rows_removed").toInt(), 3);
    EXPECT_EQ(env.value("removed").toArray().size(), 3);
    EXPECT_TRUE(env.value("scope_ids").isNull());
    const QString md = readStr(p);
    EXPECT_FALSE(md.contains(QStringLiteral("First look")));
    EXPECT_TRUE(md.contains(QStringLiteral("ANTS-0009")));   // INV-2b
}

// INV-12 — an explicitly empty scope_ids array → bad_args.
TEST(FeedbackPruneTracking, Inv12EmptyScopeBadArgs) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "prune_tracking";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["scope_ids"] = QJsonArray{};
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("bad_args"));
}

// INV-13 — prune on an absent file → not_found.
TEST(FeedbackPruneTracking, Inv13AbsentFileNotFound) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/GONE_Ants_MCP_Feedback.md";
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "prune_tracking";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("not_found"));
}

// Schema — feedback_log inputSchema declares the op + the scope_ids property.
TEST(FeedbackPruneTracking, SchemaDeclaresOpAndScopeIds) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("e.append(QStringLiteral(\"prune_tracking\"))"),
              std::string::npos);
    EXPECT_NE(ci.find("props[\"scope_ids\"]"), std::string::npos);
}

// Dispatch — cmdFeedbackLog routes the op to FeedbackFile::pruneTracking.
TEST(FeedbackPruneTracking, DispatchWired) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("op == QStringLiteral(\"prune_tracking\")"),
              std::string::npos);
    EXPECT_NE(rc.find("FeedbackFile::pruneTracking("), std::string::npos);
}
