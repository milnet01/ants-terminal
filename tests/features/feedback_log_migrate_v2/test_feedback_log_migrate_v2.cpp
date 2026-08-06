// ANTS-3446 — feature-conformance test for feedback_log op:"migrate_v2".
// Pure FeedbackFile::migrateV2 over synthetic v1 fixtures + a live
// RemoteControl::cmdFeedbackLog drive + schema/dispatch source-greps.
// See spec.md + docs/specs/ANTS-3446.md.

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
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {

// —=\xE2\x80\x94  📋=\xF0\x9F\x93\x8B  ✅=\xE2\x9C\x85
const QString kStamp =
    QStringLiteral("- **Proposed ID:** _(maintainer to assign)_");
const QString kMarkerV2 = QStringLiteral("<!-- ants-mcp-feedback: 2 -->");

// The §2.8 mixed fixture. Watermark = the `## 📋 …` heading (line 7).
//   Issue #0 (line 4)  — above watermark, finding-shaped → orphan
//   Issue #1 (line 13) — below watermark, finding-shaped → stamp
//   Positive note (16) — below watermark, prose        → unclassified
const char *kMixed =
    "<!-- ants-mcp-feedback: 1 -->\n"                                  // 1
    "# Feedback TEST\n"                                                // 2
    "\n"                                                               // 3
    "### Issue #0 \xE2\x80\x94 earlier finding, never tabled\n"        // 4
    "- **What:** an older gap.\n"                                      // 5
    "\n"                                                               // 6
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
    "(2026-06-01, maintainer)\n"                                       // 7
    "\n"                                                               // 8
    "| #N | Status | Notes |\n"                                        // 9
    "|---|---|---|\n"                                                  // 10
    "| #1 timeout | \xE2\x9C\x85 | shipped ANTS-1525 |\n"              // 11
    "\n"                                                               // 12
    "### Issue #1 \xE2\x80\x94 verify_changes timed out\n"            // 13
    "- **What:** hangs on a cold cache.\n"                            // 14
    "\n"                                                               // 15
    "### Positive note \xE2\x80\x94 apply_edits is fast\n"            // 16
    "It felt instant.\n";                                             // 17

QString mixed() { return QString::fromUtf8(kMixed); }

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
std::string slurp(const char *path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const FeedbackFile::MigrateStamp *
byHeadingContains(const QVector<FeedbackFile::MigrateStamp> &v,
                  const QString &needle) {
    for (const auto &s : v)
        if (s.heading.contains(needle)) return &s;
    return nullptr;
}

}  // namespace

// §2.8 / INV-2/3/4/5/11 — the whole mixed fixture: marker bump, one stamp under
// Issue #1, Issue #0 an orphan, Positive note unclassified, table byte-identical.
TEST(FeedbackMigrateV2, MixedFixture) {
    const FeedbackFile::MigrateResult r = FeedbackFile::migrateV2(mixed());
    EXPECT_FALSE(r.alreadyV2);

    // Exactly one stamp, one orphan, one unclassified.
    ASSERT_EQ(r.stamped.size(), 1);
    ASSERT_EQ(r.orphans.size(), 1);
    ASSERT_EQ(r.unclassified.size(), 1);

    EXPECT_TRUE(r.stamped.at(0).heading.contains(QStringLiteral("Issue #1")));
    EXPECT_EQ(r.stamped.at(0).line, 13);                         // input coord
    EXPECT_TRUE(r.orphans.at(0).heading.contains(QStringLiteral("Issue #0")));
    EXPECT_EQ(r.orphans.at(0).line, 4);
    EXPECT_EQ(r.orphans.at(0).reason,
              QStringLiteral("finding_shaped_above_watermark"));
    EXPECT_TRUE(
        r.unclassified.at(0).heading.contains(QStringLiteral("Positive note")));
    EXPECT_EQ(r.unclassified.at(0).line, 16);

    // The stamp sits directly under Issue #1's heading, above its What bullet.
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(QString::fromUtf8(
        "### Issue #1 \xE2\x80\x94 verify_changes timed out"));
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), kStamp);
    EXPECT_EQ(out.at(h + 2), QStringLiteral("- **What:** hangs on a cold cache."));

    // Marker bumped; table + Issue #0 + Positive note byte-identical & in place.
    EXPECT_TRUE(r.newContent.contains(kMarkerV2));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral(": 1 -->")));
    EXPECT_TRUE(r.newContent.contains(
        QString::fromUtf8("| #1 timeout | \xE2\x9C\x85 | shipped ANTS-1525 |")));
    // Issue #0 (above watermark) is NOT stamped.
    const int h0 = out.indexOf(QString::fromUtf8(
        "### Issue #0 \xE2\x80\x94 earlier finding, never tabled"));
    ASSERT_GE(h0, 0);
    EXPECT_EQ(out.at(h0 + 1), QStringLiteral("- **What:** an older gap."));
    // Positive note (below watermark, prose) is NOT stamped.
    EXPECT_FALSE(r.newContent.contains(
        QStringLiteral("- **Proposed ID:** _(maintainer to assign)_\n"
                       "It felt instant.")));

    // Exactly the two-line change → net bytes added (INV-11).
    EXPECT_GT(r.bytesDelta, 0);
}

// INV-6 — idempotency: feed the migrated output back → clean no-op.
TEST(FeedbackMigrateV2, Idempotent) {
    const FeedbackFile::MigrateResult r1 = FeedbackFile::migrateV2(mixed());
    const FeedbackFile::MigrateResult r2 =
        FeedbackFile::migrateV2(r1.newContent);
    EXPECT_TRUE(r2.alreadyV2);
    EXPECT_EQ(r2.stamped.size(), 0);
    EXPECT_EQ(r2.bytesDelta, 0);
    EXPECT_EQ(r2.newContent, r1.newContent);  // byte-identical
}

// §2.2 pass 2 — blank-first finding: the stamp is inserted AFTER the blank that
// follows the heading (preserve heading → blank → bullets).
TEST(FeedbackMigrateV2, BlankFirstStampAfterBlank) {
    const char *blankFirst =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #7 \xE2\x80\x94 blank first\n"
        "\n"
        "- **What:** a gap.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(blankFirst));
    ASSERT_EQ(r.stamped.size(), 1);
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    const int h = out.indexOf(
        QString::fromUtf8("### Issue #7 \xE2\x80\x94 blank first"));
    ASSERT_GE(h, 0);
    EXPECT_EQ(out.at(h + 1), QString());   // blank preserved
    EXPECT_EQ(out.at(h + 2), kStamp);      // stamp after the blank
    EXPECT_EQ(out.at(h + 3), QStringLiteral("- **What:** a gap."));
}

// INV-3 — discriminator: heading-only numbered finding stamped; bullet-only
// finding stamped; prose block left line-less.
TEST(FeedbackMigrateV2, FindingShapedDiscriminator) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Observation #3 \xE2\x80\x94 heading only, no bullets\n"
        "Some freeform prose about it.\n"
        "\n"
        "### A plain title\n"
        "- **Repro:** do the thing.\n"
        "\n"
        "### Positive note \xE2\x80\x94 nice\n"
        "It felt good.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    // Two findings stamped (Observation heading + Repro bullet); note untouched.
    EXPECT_EQ(r.stamped.size(), 2);
    EXPECT_TRUE(byHeadingContains(r.stamped, QStringLiteral("Observation #3")));
    EXPECT_TRUE(byHeadingContains(r.stamped, QStringLiteral("A plain title")));
    EXPECT_EQ(r.unclassified.size(), 1);
    EXPECT_TRUE(
        byHeadingContains(r.unclassified, QStringLiteral("Positive note")));
    // The prose note carries no stamp.
    EXPECT_FALSE(r.newContent.contains(
        QStringLiteral("### Positive note \xE2\x80\x94 nice\n") + kStamp));
}

// INV-3 — an already-tagged finding (existing Proposed-ID line) is untouched.
TEST(FeedbackMigrateV2, AlreadyTaggedUntouched) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #9 \xE2\x80\x94 already triaged\n"
        "- **Proposed ID:** ANTS-1525\n"
        "- **What:** x.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    EXPECT_EQ(r.stamped.size(), 0);
    // Only the marker changed; the id line is not duplicated.
    EXPECT_EQ(r.newContent.count(QStringLiteral("**Proposed ID:**")), 1);
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("- **Proposed ID:** ANTS-1525")));
    EXPECT_FALSE(r.newContent.contains(kStamp));
}

// §2.3 — a file with NO tracking table (watermark -1): every finding-shaped
// block is below the -1 watermark → all stamped, none orphaned.
TEST(FeedbackMigrateV2, NoWatermarkStampsAll) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #1 \xE2\x80\x94 a\n"
        "- **What:** a.\n"
        "\n"
        "### Issue #2 \xE2\x80\x94 b\n"
        "- **What:** b.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    EXPECT_EQ(r.stamped.size(), 2);
    EXPECT_EQ(r.orphans.size(), 0);
    EXPECT_EQ(r.newContent.count(kStamp), 2);
}

// INV-4 — a canonical tracking table is byte-identical after migration and the
// v1 watermark is unchanged; stamps land only below it.
TEST(FeedbackMigrateV2, TablesUntouchedWatermarkStable) {
    const FeedbackFile::ParseResult before = FeedbackFile::parse(mixed());
    const FeedbackFile::MigrateResult r = FeedbackFile::migrateV2(mixed());
    const FeedbackFile::ParseResult after =
        FeedbackFile::parse(r.newContent);
    // The maintainer heading text is preserved (its line shifts only if a stamp
    // were inserted above it — it is not).
    ASSERT_GE(before.lastMaintainerLine, 0);
    const QStringList bl = mixed().split(QLatin1Char('\n'));
    const QStringList al = r.newContent.split(QLatin1Char('\n'));
    EXPECT_EQ(al.at(after.lastMaintainerLine - 1),
              bl.at(before.lastMaintainerLine - 1));
    // Watermark line is unchanged (no stamp above it in this fixture).
    EXPECT_EQ(after.lastMaintainerLine, before.lastMaintainerLine);
    // The table rows survive verbatim.
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| #N | Status | Notes |")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("|---|---|---|")));
}

// INV-4 — migrate_v2 leaves the v1 tables/watermark in place. The v1 watermark
// (lastMaintainerLine) is byte-stable across migration; the stamped finding is
// surfaced in the migrated file's delta. NOTE: after ANTS-3448 the migrated
// `: 2` file reads under the marker-aware v2 rule, so `after.delta` is the
// un-triaged inline findings (Issue #5's stamped block) — NOT the v1 after-table
// region. The migrate invariant is the watermark preservation, not delta-start
// identity across the version boundary.
TEST(FeedbackMigrateV2, V1DeltaPreserved) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
        "(2026-06-01, maintainer)\n"
        "\n"
        "| # | Status |\n"
        "|---|---|\n"
        "| #1 | \xE2\x9C\x85 |\n"
        "\n"
        "## 2026-06-20 \xE2\x80\x94 Vestige session\n"
        "\n"
        "### Issue #5 \xE2\x80\x94 fresh\n"
        "- **What:** new gap.\n";
    const QString in = QString::fromUtf8(fix);
    const FeedbackFile::ParseResult before = FeedbackFile::parse(in);
    const FeedbackFile::MigrateResult r = FeedbackFile::migrateV2(in);
    const FeedbackFile::ParseResult after = FeedbackFile::parse(r.newContent);
    ASSERT_TRUE(before.deltaPresent);
    EXPECT_TRUE(after.deltaPresent);
    // The v1 watermark heading text is byte-stable (the table was not moved —
    // the real INV-4 property; no stamp was inserted above it in this fixture).
    ASSERT_GE(before.lastMaintainerLine, 0);
    const QStringList bl = in.split(QLatin1Char('\n'));
    const QStringList al = r.newContent.split(QLatin1Char('\n'));
    EXPECT_EQ(al.at(after.lastMaintainerLine - 1),
              bl.at(before.lastMaintainerLine - 1));
    // The below-watermark finding was stamped; the migrated file now reads under
    // v2, so its delta is Issue #5's stamped block.
    EXPECT_EQ(r.stamped.size(), 1);
    EXPECT_EQ(after.formatVersion, 2);
    EXPECT_TRUE(after.delta.contains(kStamp));
    EXPECT_TRUE(after.delta.contains(QStringLiteral("Issue #5")));
}

// INV-2 — a legacy file with NO marker gets one inserted as line 1.
TEST(FeedbackMigrateV2, MarkerAbsentInserted) {
    const char *fix =
        "# DOOM feedback\n"
        "\n"
        "### Issue #1 \xE2\x80\x94 x\n"
        "- **What:** y.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    EXPECT_EQ(out.at(0), kMarkerV2);
    EXPECT_EQ(out.at(1), QStringLiteral("# DOOM feedback"));
    EXPECT_EQ(r.stamped.size(), 1);
}

// §2.2 pass 1 — a malformed marker (no digit) is replaced, not left as junk.
TEST(FeedbackMigrateV2, MalformedMarkerRepaired) {
    const char *fix =
        "<!-- ants-mcp-feedback: -->\n"
        "# T\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    EXPECT_FALSE(r.alreadyV2);
    EXPECT_TRUE(r.newContent.contains(kMarkerV2));
    EXPECT_FALSE(r.newContent.contains(QStringLiteral("<!-- ants-mcp-feedback: -->")));
    // Exactly one marker line (no duplicate inserted).
    EXPECT_EQ(r.newContent.count(QStringLiteral("ants-mcp-feedback:")), 1);
}

// §2.2 pass 1 — a marker NOT on line 1 is replaced in place, not duplicated.
TEST(FeedbackMigrateV2, MisplacedMarkerReplacedInPlace) {
    const char *fix =
        "# T\n"
        "<!-- ants-mcp-feedback: 1 -->\n"
        "\n"
        "### Issue #1 \xE2\x80\x94 x\n"
        "- **What:** y.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    EXPECT_EQ(r.newContent.count(QStringLiteral("ants-mcp-feedback:")), 1);
    EXPECT_TRUE(r.newContent.contains(kMarkerV2));
    const QStringList out = r.newContent.split(QLatin1Char('\n'));
    EXPECT_EQ(out.at(0), QStringLiteral("# T"));       // H1 stays first
    EXPECT_EQ(out.at(1), kMarkerV2);                   // marker replaced in place
}

// INV-10 — a `### `/table/`**Proposed ID:**` line inside a fence is inert.
TEST(FeedbackMigrateV2, FenceSafety) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #1 \xE2\x80\x94 real\n"
        "- **What:** example:\n"
        "```text\n"
        "### Issue #FAKE\n"
        "- **Proposed ID:** ANTS-0000\n"
        "```\n"
        "- **Impact:** z.\n";
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    // Only the real finding is enumerated + stamped; the fenced one is inert.
    EXPECT_EQ(r.stamped.size(), 1);
    EXPECT_EQ(r.newContent.count(kStamp), 1);
    // The fenced sample text is untouched.
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("### Issue #FAKE")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("- **Proposed ID:** ANTS-0000")));
}

// ---- live wrapper drives --------------------------------------------------

// INV-7 — dry_run leaves the file byte-identical; the envelope still reports.
TEST(FeedbackMigrateV2, LiveDryRunNoWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, mixed()));
    const QString before = readStr(p);
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "migrate_v2";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    req["dry_run"] = true;
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_TRUE(env.value("dry_run").toBool());
    EXPECT_EQ(env.value("stamped_count").toInt(), 1);
    EXPECT_EQ(env.value("orphans").toArray().size(), 1);
    EXPECT_EQ(env.value("unclassified").toArray().size(), 1);
    EXPECT_EQ(readStr(p), before);   // untouched
}

// INV-9 — a real run writes a `: 2` file with the stamp.
TEST(FeedbackMigrateV2, LiveRealWrite) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, mixed()));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "migrate_v2";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_FALSE(env.value("already_v2").toBool());
    EXPECT_EQ(env.value("stamped_count").toInt(), 1);
    const QString md = readStr(p);
    EXPECT_TRUE(md.contains(kMarkerV2));
    EXPECT_TRUE(md.contains(kStamp));
    EXPECT_TRUE(md.contains(QStringLiteral("| #1 timeout")));  // table in place

    // Second live run is a clean no-op.
    const QJsonObject env2 = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env2.value("ok").toBool());
    EXPECT_TRUE(env2.value("already_v2").toBool());
    EXPECT_EQ(env2.value("stamped_count").toInt(), 0);
    EXPECT_EQ(readStr(p), md);   // byte-identical
}

// INV-9 — an absent file refuses not_found.
TEST(FeedbackMigrateV2, LiveAbsentFileNotFound) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/GONE_Ants_MCP_Feedback.md";
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["op"] = "migrate_v2";
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), QStringLiteral("not_found"));
}

// Schema — feedback_log inputSchema enumerates the op.
TEST(FeedbackMigrateV2, SchemaDeclaresOp) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("e.append(QStringLiteral(\"migrate_v2\"))"),
              std::string::npos);
}

// Dispatch — cmdFeedbackLog routes the op to FeedbackFile::migrateV2.
TEST(FeedbackMigrateV2, DispatchWired) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("op == QStringLiteral(\"migrate_v2\")"),
              std::string::npos);
    EXPECT_NE(rc.find("FeedbackFile::migrateV2("), std::string::npos);
}

// ANTS-3474 — backfill inline Proposed-IDs from the file's own v1 tracking
// tables. Two findings, each carrying a BLANK placeholder (the append_finding
// default); a canonical tracking table maps one to ANTS-3400, the other
// (audio) matches nothing → stays blank (precision over recall).
TEST(FeedbackMigrateV2, Ants3474BackfillFromTracking) {
    const QString v1 = QString::fromUtf8(
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# Feedback TEST\n"
        "\n"
        "### roadmap_query rejects status planned vocabulary mismatch\n"
        "- **What:** the query verb refused status=planned.\n"
        "- **Proposed ID:** _(maintainer to assign)_\n"
        "\n"
        "### audio buffer underruns cause playback glitches\n"
        "- **What:** the ring buffer drains.\n"
        "- **Proposed ID:** _(maintainer to assign)_\n"
        "\n"
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
        "(2026-07-01, maintainer)\n"
        "\n"
        "| Item | ID(s) | Status | Notes |\n"
        "|------|-------|--------|-------|\n"
        "| roadmap_query rejects status planned read write vocabulary "
        "mismatch | ANTS-3400 | \xE2\x9C\x85 | shipped |\n");

    // Backfill ON — the roadmap_query finding gets ANTS-3400 inline; the
    // unrelated audio finding keeps its blank placeholder.
    const FeedbackFile::MigrateResult on = FeedbackFile::migrateV2(v1, true);
    ASSERT_EQ(on.backfilled.size(), 1);
    EXPECT_EQ(on.backfilled.at(0).id, QStringLiteral("ANTS-3400"));
    EXPECT_GE(on.backfilled.at(0).confidencePct, 66);
    EXPECT_TRUE(on.newContent.contains(
        QStringLiteral("- **Proposed ID:** ANTS-3400")));
    EXPECT_EQ(on.newContent.count(kStamp), 1);   // only audio left blank

    // Backfill OFF (default) — no id carried in; both placeholders survive.
    const FeedbackFile::MigrateResult off = FeedbackFile::migrateV2(v1, false);
    EXPECT_EQ(off.backfilled.size(), 0);
    EXPECT_FALSE(off.newContent.contains(
        QStringLiteral("- **Proposed ID:** ANTS-3400")));
    EXPECT_EQ(off.newContent.count(kStamp), 2);
}

// ANTS-3508 — §2.3 precondition: a NON-canonical `## Status of prior items (…)`
// heading (no 📋, different phrase → fails maintainerAnchorRe) must NOT set the
// watermark. The finding sits ABOVE that heading: if the heading wrongly set the
// watermark, the finding would be an orphan; because it does not, the watermark
// stays -1 and the finding is stamped. The non-canonical table is left in place.
TEST(FeedbackMigrateV2, NonCanonicalAnchorNoWatermark) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"                              // 1
        "# T\n"                                                        // 2
        "\n"                                                           // 3
        "### Issue #1 \xE2\x80\x94 fresh, above the status recap\n"    // 4
        "- **What:** a gap.\n"                                         // 5
        "\n"                                                           // 6
        "## Status of prior items (2026-06-01, maintainer)\n"          // 7
        "\n"                                                           // 8
        "| Item | Status |\n"                                          // 9
        "|---|---|\n"                                                  // 10
        "| old thing | done |\n";                                      // 11
    const QString in = QString::fromUtf8(fix);
    // The non-canonical heading is not recognised as a tracking block.
    EXPECT_EQ(FeedbackFile::parse(in).lastMaintainerLine, -1);

    const FeedbackFile::MigrateResult r = FeedbackFile::migrateV2(in);
    // Watermark -1 ⇒ the finding above the recap is still "below" ⇒ stamped,
    // not orphaned (proves the recap heading did not move the watermark).
    ASSERT_EQ(r.stamped.size(), 1);
    EXPECT_TRUE(r.stamped.at(0).heading.contains(QStringLiteral("Issue #1")));
    EXPECT_EQ(r.orphans.size(), 0);
    EXPECT_EQ(r.newContent.count(kStamp), 1);
    // The non-canonical heading + its table survive verbatim, in place.
    EXPECT_TRUE(r.newContent.contains(
        QStringLiteral("## Status of prior items (2026-06-01, maintainer)")));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("| old thing | done |")));
}

// ANTS-3508 — §2.4 / feedbackfile.cpp:1159: an ABOVE-watermark `### ` block that
// is NOT finding-shaped (prose) is left line-less AND reported in NEITHER
// orphans[] nor unclassified[] (expected already-reviewed prose). The below-
// watermark finding is stamped as usual; only the prose block exercises the
// "not-shaped & above → not reported" branch.
TEST(FeedbackMigrateV2, AboveWatermarkProseUnreported) {
    const char *fix =
        "<!-- ants-mcp-feedback: 1 -->\n"                              // 1
        "# T\n"                                                        // 2
        "\n"                                                           // 3
        "### Positive note \xE2\x80\x94 apply_edits felt instant\n"    // 4
        "It just worked.\n"                                            // 5
        "\n"                                                           // 6
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
        "(2026-06-01, maintainer)\n"                                   // 7
        "\n"                                                           // 8
        "| # | Status |\n"                                            // 9
        "|---|---|\n"                                                  // 10
        "| #1 | \xE2\x9C\x85 |\n"                                     // 11
        "\n"                                                           // 12
        "### Issue #1 \xE2\x80\x94 below, fresh\n"                    // 13
        "- **What:** a gap.\n";                                       // 14
    const FeedbackFile::MigrateResult r =
        FeedbackFile::migrateV2(QString::fromUtf8(fix));
    // Below-watermark finding stamped; the above-watermark prose is silent.
    ASSERT_EQ(r.stamped.size(), 1);
    EXPECT_TRUE(r.stamped.at(0).heading.contains(QStringLiteral("Issue #1")));
    EXPECT_EQ(r.orphans.size(), 0);        // prose ≠ finding_shaped_above
    EXPECT_EQ(r.unclassified.size(), 0);   // prose above ≠ freeform below
    // The prose block carries no stamp; its heading + body survive verbatim.
    EXPECT_FALSE(r.newContent.contains(
        QString::fromUtf8("### Positive note \xE2\x80\x94 apply_edits felt "
                          "instant\n") + kStamp));
    EXPECT_TRUE(r.newContent.contains(QStringLiteral("It just worked.")));
    EXPECT_EQ(r.newContent.count(kStamp), 1);
}
