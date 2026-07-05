// ANTS-3443 — feature-conformance test for feedback_log op:"compact_resolved".
// Pure FeedbackFile::compactResolved over synthetic v2 fixtures (injected
// shippedIds/roadmapIds) + a live RemoteControl::cmdFeedbackLog drive against
// a mock ROADMAP.md + schema/dispatch source-greps. See spec.md +
// docs/specs/ANTS-3443.md.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
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

// —=\xE2\x80\x94  →=\xE2\x86\x92  ✅=\xE2\x9C\x85
// The §2.10 worked example: one collapse + four distinct skip outcomes +
// one id-less prose note.
const char *kFixture =
    "<!-- ants-mcp-feedback: 2 -->\n"
    "# Ants MCP Feedback TEST\n"
    "\n"
    "> Contributors append below.\n"
    "\n"
    "## 2026-06-20 \xE2\x80\x94 Vestige session\n"
    "\n"
    "### Issue #1 \xE2\x80\x94 verify_changes timed out\n"
    "- **Proposed ID:** ANTS-1525, ANTS-1579\n"
    "- **What:** the verb hangs past 5 s on a cold cache.\n"
    "- **Repro:** first call after relaunch.\n"
    "- **Impact:** blocks the batch.\n"
    "\n"
    "### Issue #2 \xE2\x80\x94 codebase_index missing counts\n"
    "- **Proposed ID:** ANTS-1600\n"
    "- **What:** counts absent on the section rows.\n"
    "\n"
    "### Issue #3 \xE2\x80\x94 typo in error text\n"
    "- **Proposed ID:** ANTS-9999\n"
    "- **What:** occured -> occurred.\n"
    "\n"
    "### Issue #4 \xE2\x80\x94 duplicate of the timeout\n"
    "- **Proposed ID:** n/a \xE2\x80\x94 folded into ANTS-1525\n"
    "- **What:** same root cause as #1.\n"
    "\n"
    "### Positive note \xE2\x80\x94 apply_edits is fast\n"
    "It felt instant on a 2 k-line file. Nice.\n";

QString fx() { return QString::fromUtf8(kFixture); }

const QString kBreadcrumb = QString::fromUtf8(
    "\xE2\x86\x92 shipped \xE2\x9C\x85 (write-up compacted, ANTS-3443)");

// roadmapIds = {1525,1579,1600}; shippedIds = {1525,1579}; 9999 absent.
FeedbackFile::ResolveOptions stdOpts() {
    FeedbackFile::ResolveOptions o;
    o.roadmapIds = { QStringLiteral("ANTS-1525"), QStringLiteral("ANTS-1579"),
                     QStringLiteral("ANTS-1600") };
    o.shippedIds = { QStringLiteral("ANTS-1525"), QStringLiteral("ANTS-1579") };
    return o;
}

const FeedbackFile::ResolvedFinding *
byHeadingContains(const FeedbackFile::ResolveResult &r, const QString &needle) {
    for (const auto &f : r.findings)
        if (f.heading.contains(needle)) return &f;
    return nullptr;
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

// Mock ROADMAP.md in the real ants-v1 format: `- <emoji> [ANTS-NNNN] **head**`.
// 1525/1579 ✅, 1600 🚧 (open), 9999 absent.
const char *kRoadmap =
    "# Roadmap\n"
    "\n"
    "- \xE2\x9C\x85 [ANTS-1525] **verify_changes timeout fixed.**\n"
    "- \xE2\x9C\x85 [ANTS-1579] **cache warm on relaunch.**\n"
    "- \xF0\x9F\x9A\xA7 [ANTS-1600] **codebase_index counts.**\n";

// Seed a temp dir with a v2 feedback file + a ROADMAP.md; return the fb path.
QString seed(QTemporaryDir &dir, bool withRoadmap = true) {
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    writeStr(p, fx());
    if (withRoadmap) writeStr(dir.path() + "/ROADMAP.md",
                              QString::fromUtf8(kRoadmap));
    return p;
}

std::string slurp(const char *path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// §2.10 / INV-2/4/7/8/12 — one collapse, three distinct skips, id-less note
// is not a finding.
TEST(FeedbackCompactResolved, MixedBlockGating) {
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(fx(), stdOpts());
    // Four findings (the id-less "Positive note" is NOT one).
    ASSERT_EQ(r.findings.size(), 4);
    for (const auto &f : r.findings)
        EXPECT_FALSE(f.heading.contains(QStringLiteral("Positive note")));

    const auto *i1 = byHeadingContains(r, QStringLiteral("Issue #1"));
    const auto *i2 = byHeadingContains(r, QStringLiteral("Issue #2"));
    const auto *i3 = byHeadingContains(r, QStringLiteral("Issue #3"));
    const auto *i4 = byHeadingContains(r, QStringLiteral("Issue #4"));
    ASSERT_TRUE(i1 && i2 && i3 && i4);

    EXPECT_TRUE(i1->collapsed);
    EXPECT_EQ(i1->code, QString());

    EXPECT_FALSE(i2->collapsed);
    EXPECT_EQ(i2->code, QStringLiteral("has_open_id"));
    EXPECT_EQ(i2->openIds, QStringList{ QStringLiteral("ANTS-1600") });

    EXPECT_FALSE(i3->collapsed);
    EXPECT_EQ(i3->code, QStringLiteral("roadmap_unresolved_ids"));
    EXPECT_EQ(i3->unresolvedIds, QStringList{ QStringLiteral("ANTS-9999") });

    EXPECT_FALSE(i4->collapsed);
    EXPECT_EQ(i4->code, QStringLiteral("no_shippable_id"));
    // n/a closure still surfaces the incidental id it names.
    EXPECT_EQ(i4->ids, QStringList{ QStringLiteral("ANTS-1525") });
}

// §2.7 / INV-3 — the collapsed stub is exactly heading → blank → retained id
// line → breadcrumb → blank, and the write-up bullets are gone.
TEST(FeedbackCompactResolved, StubShapeAndRetainedId) {
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(fx(), stdOpts());
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    int h = out.indexOf(QString::fromUtf8("### Issue #1 \xE2\x80\x94 "
                                          "verify_changes timed out"));
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QString());                       // blank
    EXPECT_EQ(out.at(h + 2),
              QStringLiteral("- **Proposed ID:** ANTS-1525, ANTS-1579"));
    EXPECT_EQ(out.at(h + 3), kBreadcrumb);                     // breadcrumb
    EXPECT_EQ(out.at(h + 4), QString());                       // trailing blank
    // The verbose body is gone; the un-collapsed findings are intact.
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("the verb hangs past")));
    EXPECT_TRUE(r.newContent.contains(
        QStringLiteral("counts absent on the section rows")));
    EXPECT_TRUE(r.newContent.contains(
        QStringLiteral("- **Proposed ID:** n/a")));
    // exactly one finding collapsed.
    int collapsed = 0;
    for (const auto &f : r.findings) if (f.collapsed) ++collapsed;
    EXPECT_EQ(collapsed, 1);
}

// INV-3 — the Proposed-ID line is lifted from *below* the What/Repro bullets
// to directly under the heading; a second id line is dropped.
TEST(FeedbackCompactResolved, ProposedIdLiftedAndSecondDropped) {
    const char *below =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Below \xE2\x80\x94 id under bullets\n"
        "- **What:** something.\n"
        "- **Repro:** steps.\n"
        "- **Proposed ID:** ANTS-1525\n"
        "- **Impact:** later.\n"
        "- **Proposed ID:** ANTS-1579\n";
    FeedbackFile::ResolveOptions o;
    o.roadmapIds = { QStringLiteral("ANTS-1525"), QStringLiteral("ANTS-1579") };
    o.shippedIds = { QStringLiteral("ANTS-1525"), QStringLiteral("ANTS-1579") };
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(QString::fromUtf8(below), o);
    ASSERT_EQ(r.findings.size(), 1);
    EXPECT_TRUE(r.findings.at(0).collapsed);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(
        QString::fromUtf8("### Below \xE2\x80\x94 id under bullets"));
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **Proposed ID:** ANTS-1525"));
    EXPECT_EQ(out.at(h + 3), kBreadcrumb);
    // Only ONE Proposed-ID line survives (the first); the What/Impact gone.
    EXPECT_EQ(r.newContent.count(QStringLiteral("**Proposed ID:**")), 1);
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("something.")));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("ANTS-1579")));
}

// INV-2 — a multi-id finding with only one id ✅ is has_open_id, not collapsed.
TEST(FeedbackCompactResolved, MultiIdPartialOpen) {
    FeedbackFile::ResolveOptions o;
    o.roadmapIds = { QStringLiteral("ANTS-1525"), QStringLiteral("ANTS-1579") };
    o.shippedIds = { QStringLiteral("ANTS-1525") };  // 1579 open
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(fx(), o);
    const auto *i1 = byHeadingContains(r, QStringLiteral("Issue #1"));
    ASSERT_TRUE(i1);
    EXPECT_FALSE(i1->collapsed);
    EXPECT_EQ(i1->code, QStringLiteral("has_open_id"));
    EXPECT_EQ(i1->openIds, QStringList{ QStringLiteral("ANTS-1579") });
}

// INV-6 — idempotency: the collapsed output collapses nothing on a second run;
// every stub trips already_compacted (checked before the roadmap gates) even
// when the id is reopened ✅→(removed from shipped).
TEST(FeedbackCompactResolved, IdempotentEvenWhenReopened) {
    const FeedbackFile::ResolveResult r1 =
        FeedbackFile::compactResolved(fx(), stdOpts());
    // Re-run with 1525 REOPENED (dropped from shippedIds) — still no re-touch.
    FeedbackFile::ResolveOptions reopened = stdOpts();
    reopened.shippedIds.remove(QStringLiteral("ANTS-1525"));
    reopened.shippedIds.remove(QStringLiteral("ANTS-1579"));
    const FeedbackFile::ResolveResult r2 =
        FeedbackFile::compactResolved(r1.newContent, reopened);
    const auto *i1 = byHeadingContains(r2, QStringLiteral("Issue #1"));
    ASSERT_TRUE(i1);
    EXPECT_FALSE(i1->collapsed);
    EXPECT_EQ(i1->code, QStringLiteral("already_compacted"));
    EXPECT_EQ(r2.newContent, r1.newContent);  // byte-identical
    EXPECT_EQ(r2.bytesSaved, 0);
}

// INV-5 — a collapsed finding retains a shippable id, so the shared enumerator
// still reports its id line after the collapse (stays out of the delta).
TEST(FeedbackCompactResolved, DeltaInvarianceRetainsId) {
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(fx(), stdOpts());
    const QStringList lines = r.newContent.split(QLatin1Char('\n'));
    const QVector<FeedbackFile::FindingBlock> blocks =
        FeedbackFile::enumerateFindingBlocks(lines);
    bool found = false;
    for (const auto &b : blocks) {
        if (b.heading.contains(QStringLiteral("Issue #1"))) {
            found = true;
            EXPECT_GE(b.idLine0, 0);
            EXPECT_TRUE(b.idValue.contains(QStringLiteral("ANTS-1525")));
        }
    }
    EXPECT_TRUE(found);
}

// INV-8 — a `### ` heading and a `→ shipped` line pasted inside a fence are
// ignored: the block enumerates as ONE finding and collapses (the fenced
// `→ shipped` does not trip already_compacted).
TEST(FeedbackCompactResolved, FenceSafety) {
    const char *fenced =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Real finding\n"
        "- **Proposed ID:** ANTS-1525\n"
        "- **What:** example follows:\n"
        "```text\n"
        "### Active\n"
        "\xE2\x86\x92 shipped nonsense\n"
        "```\n"
        "- **Impact:** x.\n";
    FeedbackFile::ResolveOptions o;
    o.roadmapIds = { QStringLiteral("ANTS-1525") };
    o.shippedIds = { QStringLiteral("ANTS-1525") };
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(QString::fromUtf8(fenced), o);
    ASSERT_EQ(r.findings.size(), 1);   // ### Active inside the fence is inert
    EXPECT_TRUE(r.findings.at(0).collapsed);
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("nonsense")));
}

// INV-13 — a present v2 file with nothing collapsible → collapsed:0,
// bytes_saved:0, and every finding reported in skipped[].
TEST(FeedbackCompactResolved, NothingCollapsibleIsCleanNoOp) {
    FeedbackFile::ResolveOptions o;  // empty roadmap → all unresolved
    const FeedbackFile::ResolveResult r =
        FeedbackFile::compactResolved(fx(), o);
    int collapsed = 0;
    for (const auto &f : r.findings) if (f.collapsed) ++collapsed;
    EXPECT_EQ(collapsed, 0);
    EXPECT_EQ(r.bytesSaved, 0);
    EXPECT_EQ(r.newContent, fx());   // byte-identical
}

// ---- live wrapper drives (real ROADMAP.md resolution) ---------------------

// dry_run leaves the file byte-identical; envelope reports the collapse.
TEST(FeedbackCompactResolved, LiveDryRunNoWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    const QString before = readStr(p);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "compact_resolved";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["dry_run"] = true;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("findings_collapsed").toInt(), 1);
    EXPECT_TRUE(env.value("dry_run").toBool());
    EXPECT_EQ(env.value("collapsed").toArray().size(), 1);
    EXPECT_EQ(env.value("skipped").toArray().size(), 3);
    EXPECT_EQ(readStr(p), before);   // untouched
}

// A real run writes the stub; the envelope is driven by the live roadmap.
TEST(FeedbackCompactResolved, LiveRealWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "compact_resolved";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("findings_collapsed").toInt(), 1);
    const QString md = readStr(p);
    EXPECT_TRUE(md.contains(kBreadcrumb));
    EXPECT_FALSE(md.contains(QStringLiteral("the verb hangs past")));
    EXPECT_TRUE(md.contains(QStringLiteral("counts absent on the section rows")));
}

// not_v2 — a `: 1`-marked file is refused (run migrate_v2 first).
TEST(FeedbackCompactResolved, LiveNotV2) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    QString v1 = fx();
    v1.replace(QStringLiteral("<!-- ants-mcp-feedback: 2 -->"),
               QStringLiteral("<!-- ants-mcp-feedback: 1 -->"));
    writeStr(p, v1);
    writeStr(dir.path() + "/ROADMAP.md", QString::fromUtf8(kRoadmap));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "compact_resolved";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("not_v2"));
}

// not_found — an absent file is refused before any roadmap work.
TEST(FeedbackCompactResolved, LiveAbsentFileNotFound) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/GONE_Ants_MCP_Feedback.md";
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "compact_resolved";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("not_found"));
}

// roadmap_unavailable — a present v2 file but no resolvable ROADMAP.md.
TEST(FeedbackCompactResolved, LiveRoadmapUnavailable) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = seed(dir, /*withRoadmap=*/false);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "compact_resolved";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(),
              QStringLiteral("roadmap_unavailable"));
}

// Schema — feedback_log inputSchema enumerates the op.
TEST(FeedbackCompactResolved, SchemaDeclaresOp) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("e.append(QStringLiteral(\"compact_resolved\"))"),
              std::string::npos);
}

// Dispatch — cmdFeedbackLog routes the op to FeedbackFile::compactResolved.
TEST(FeedbackCompactResolved, DispatchWired) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("op == QStringLiteral(\"compact_resolved\")"),
              std::string::npos);
    EXPECT_NE(rc.find("FeedbackFile::compactResolved("), std::string::npos);
}
