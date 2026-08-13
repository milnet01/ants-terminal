// ANTS-3448 — feature-conformance test for the marker-aware v2 un-triaged
// delta in FeedbackFile::parse() + feedback_query. Pure parse() over v1/v2
// fixtures + a live RemoteControl::cmdFeedbackQuery drive + schema greps.
// See spec.md + docs/specs/ANTS-3448.md.

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

// em-dash=\xE2\x80\x94  📋=\xF0\x9F\x93\x8B  ✅=\xE2\x9C\x85

// The §2.5 fixture: a migrated `: 2` file with a retained v1 table under a
// maintainer heading, a `## ` session heading, then a filled finding, a blank
// finding, a prose note, and an un-tagged finding-shaped block.
const char *kV2 =
    "<!-- ants-mcp-feedback: 2 -->\n"                              // 1
    "# Feedback TEST\n"                                            // 2
    "\n"                                                           // 3
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
    "(2026-06-01, maintainer)\n"                                   // 4
    "\n"                                                           // 5
    "| Item | ID | Status |\n"                                     // 6
    "|---|---|---|\n"                                              // 7
    "| timeout | ANTS-1400 | \xE2\x9C\x85 |\n"                     // 8
    "\n"                                                           // 9
    "## 2026-06-20 \xE2\x80\x94 Vestige session\n"                // 10
    "\n"                                                           // 11
    "### Issue #1 \xE2\x80\x94 filled\n"                          // 12
    "- **Proposed ID:** ANTS-1525\n"                             // 13
    "- **What:** done.\n"                                        // 14
    "\n"                                                          // 15
    "### Issue #2 \xE2\x80\x94 still blank\n"                    // 16
    "- **Proposed ID:** _(maintainer to assign)_\n"             // 17
    "- **What:** open.\n"                                       // 18
    "\n"                                                        // 19
    "### Positive note \xE2\x80\x94 nice\n"                     // 20
    "It felt fast.\n"                                           // 21
    "\n"                                                        // 22
    "### Issue #3 \xE2\x80\x94 forgot to tag\n"                // 23
    "- **What:** a real gap, no id line.\n";                   // 24

QString v2() { return QString::fromUtf8(kV2); }

// The same structure without the marker (v1) — the post-table findings sit
// under the `## 2026-06-20` session heading so the v1 `#`/`## ` delta is real.
const char *kV1 =
    "<!-- ants-mcp-feedback: 1 -->\n"
    "# Feedback TEST\n"
    "\n"
    "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking "
    "(2026-06-01, maintainer)\n"
    "\n"
    "| Item | ID | Status |\n"
    "|---|---|---|\n"
    "| timeout | ANTS-1400 | \xE2\x9C\x85 |\n"
    "\n"
    "## 2026-06-20 \xE2\x80\x94 Vestige session\n"
    "\n"
    "### Issue #A \xE2\x80\x94 fresh\n"
    "- **What:** a new gap.\n";

QString v1() { return QString::fromUtf8(kV1); }

bool writeStr(const QString &path, const QString &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray u = body.toUtf8();
    const bool ok = (f.write(u) == u.size());
    f.close();
    return ok;
}
std::string slurp(const char *path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// INV-1 — version detection drives the rule.
TEST(FeedbackV2Delta, VersionDetection) {
    EXPECT_EQ(FeedbackFile::markerVersion(v2()), 2);
    EXPECT_EQ(FeedbackFile::markerVersion(v1()), 1);
    EXPECT_EQ(FeedbackFile::markerVersion(QStringLiteral("# no marker\n")), 0);
    EXPECT_EQ(FeedbackFile::markerVersion(
                  QStringLiteral("<!-- ants-mcp-feedback: -->\n")), 0);
    EXPECT_EQ(FeedbackFile::markerVersion(
                  QStringLiteral("<!-- ants-mcp-feedback: 3 -->\n")), 3);
    EXPECT_EQ(FeedbackFile::parse(v2()).formatVersion, 2);
    EXPECT_EQ(FeedbackFile::parse(v1()).formatVersion, 1);
}

// INV-3 / §2.5 — the v2 delta is exactly Issue #2's block.
TEST(FeedbackV2Delta, V2Delta) {
    const FeedbackFile::ParseResult r = FeedbackFile::parse(v2());
    ASSERT_TRUE(r.deltaPresent);
    EXPECT_EQ(r.deltaStartLine, 16);          // Issue #2 heading line
    EXPECT_EQ(r.deltaLineCount, 3);           // heading + id line + What
    EXPECT_TRUE(r.delta.contains(QStringLiteral("Issue #2")));
    EXPECT_TRUE(r.delta.contains(QStringLiteral("_(maintainer to assign)_")));
    // Issue #1 (id) and Issue #3 (no id line) are NOT in the delta.
    EXPECT_FALSE(r.delta.contains(QStringLiteral("Issue #1")));
    EXPECT_FALSE(r.delta.contains(QStringLiteral("Issue #3")));
    // No trailing blank padding.
    EXPECT_FALSE(r.delta.endsWith(QLatin1Char('\n')));
}

// INV-3/11 — concatenation of two non-contiguous un-triaged findings, and the
// delta is NOT the file slice from deltaStartLine.
TEST(FeedbackV2Delta, ConcatNonContiguous) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"       // 1
        "# T\n"                                  // 2
        "\n"                                     // 3
        "### A \xE2\x80\x94 blank\n"             // 4  un-triaged
        "- **Proposed ID:** _(maintainer to assign)_\n"  // 5
        "- **What:** a.\n"                       // 6
        "\n"                                     // 7
        "### B \xE2\x80\x94 filled\n"            // 8  triaged
        "- **Proposed ID:** ANTS-9\n"           // 9
        "- **What:** b.\n"                       // 10
        "\n"                                     // 11
        "### C \xE2\x80\x94 blank\n"             // 12 un-triaged
        "- **Proposed ID:**\n"                   // 13 (empty value)
        "- **What:** c.\n";                      // 14
    const FeedbackFile::ParseResult r =
        FeedbackFile::parse(QString::fromUtf8(fix));
    ASSERT_TRUE(r.deltaPresent);
    EXPECT_EQ(r.deltaStartLine, 4);             // first un-triaged (A)
    EXPECT_EQ(r.deltaLineCount, 6);             // A: 3 + C: 3
    EXPECT_TRUE(r.delta.contains(QStringLiteral("### A")));
    EXPECT_TRUE(r.delta.contains(QStringLiteral("### C")));
    EXPECT_FALSE(r.delta.contains(QStringLiteral("### B")));   // triaged excluded

    // INV-11: the delta is NOT the contiguous file slice from deltaStartLine.
    const QStringList lines = QString::fromUtf8(fix).split(QLatin1Char('\n'));
    QStringList slice;
    for (int i = r.deltaStartLine - 1;
         i < r.deltaStartLine - 1 + r.deltaLineCount && i < lines.size(); ++i)
        slice.append(lines.at(i));
    EXPECT_NE(r.delta, slice.join(QLatin1Char('\n')));
}

// INV-3 — a finding with an n/a closure is excluded from the delta.
TEST(FeedbackV2Delta, ClosureExcluded) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### X\n"
        "- **Proposed ID:** n/a \xE2\x80\x94 folded\n"
        "- **What:** x.\n";
    const FeedbackFile::ParseResult r =
        FeedbackFile::parse(QString::fromUtf8(fix));
    EXPECT_FALSE(r.deltaPresent);   // the only finding is closed → nothing pending
}

// INV-4 — v2 mapped ids: inline only; table id excluded; closure contributes 0.
TEST(FeedbackV2Delta, MappedIds) {
    // §2.5 fixture: only ANTS-1525 (Issue #1's inline id). ANTS-1400 (table) out.
    const FeedbackFile::ParseResult r = FeedbackFile::parse(v2());
    ASSERT_EQ(r.mappedIds.size(), 1);
    EXPECT_EQ(r.mappedIds.at(0), QStringLiteral("ANTS-1525"));
    EXPECT_FALSE(r.mappedIds.contains(QStringLiteral("ANTS-1400")));

    // Two-id union from one finding + a closure-with-embedded-id contributes 0.
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### A\n"
        "- **Proposed ID:** ANTS-1525, ANTS-1579\n"
        "- **What:** a.\n"
        "\n"
        "### B\n"
        "- **Proposed ID:** n/a \xE2\x80\x94 folded into ANTS-9999\n"
        "- **What:** b.\n";
    const FeedbackFile::ParseResult r2 =
        FeedbackFile::parse(QString::fromUtf8(fix));
    EXPECT_EQ(r2.mappedIds.size(), 2);
    EXPECT_TRUE(r2.mappedIds.contains(QStringLiteral("ANTS-1525")));
    EXPECT_TRUE(r2.mappedIds.contains(QStringLiteral("ANTS-1579")));
    EXPECT_FALSE(r2.mappedIds.contains(QStringLiteral("ANTS-9999")));  // closure
}

// INV-5 — suspected_untagged: Issue #3 (no id, has What bullet) is listed;
// the Positive note prose block is not.
TEST(FeedbackV2Delta, SuspectedUntagged) {
    const FeedbackFile::ParseResult r = FeedbackFile::parse(v2());
    ASSERT_EQ(r.suspectedUntagged.size(), 1);
    EXPECT_TRUE(r.suspectedUntagged.at(0).heading.contains(QStringLiteral("Issue #3")));
    EXPECT_EQ(r.suspectedUntagged.at(0).line, 23);
    for (const auto &s : r.suspectedUntagged)
        EXPECT_FALSE(s.heading.contains(QStringLiteral("Positive note")));
}

// INV-2 — v1 unchanged: the marker-absent/`: 1` delta is byte-identical to the
// pre-change behaviour (the after-table region), suspected empty.
TEST(FeedbackV2Delta, V1Unchanged) {
    const FeedbackFile::ParseResult r = FeedbackFile::parse(v1());
    EXPECT_EQ(r.formatVersion, 1);
    EXPECT_TRUE(r.suspectedUntagged.isEmpty());
    ASSERT_TRUE(r.deltaPresent);
    // v1 delta starts at the `## 2026-06-20` session heading (first contributor
    // heading after the watermark), engulfing the `### Issue #A` block.
    EXPECT_TRUE(r.delta.contains(QStringLiteral("Vestige session")));
    EXPECT_TRUE(r.delta.contains(QStringLiteral("Issue #A")));
    // v1 mapped ids come from the maintainer table (ANTS-1400).
    EXPECT_TRUE(r.mappedIds.contains(QStringLiteral("ANTS-1400")));
}

// INV-6 — the v1 boundary scan is version-independent (same tables read on
// `: 1` and `: 2`).
TEST(FeedbackV2Delta, BoundaryScanVersionIndependent) {
    const FeedbackFile::ParseResult rv2 = FeedbackFile::parse(v2());
    EXPECT_EQ(rv2.maintainerBlockCount, 1);
    EXPECT_EQ(rv2.lastMaintainerLine, 4);       // the `## 📋 …` heading
    ASSERT_EQ(rv2.trackingRows.size(), 1);
    EXPECT_EQ(rv2.trackingRows.at(0).ids, QStringList{QStringLiteral("ANTS-1400")});
}

// INV-7 — fence safety: a fenced `### `/`**Proposed ID:**` is inert.
TEST(FeedbackV2Delta, FenceSafety) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "### Real\n"
        "- **Proposed ID:** _(maintainer to assign)_\n"
        "- **What:** example:\n"
        "```text\n"
        "### Fake\n"
        "- **What:** fenced.\n"
        "```\n";
    const QString in = QString::fromUtf8(fix);
    const FeedbackFile::ParseResult r = FeedbackFile::parse(in);
    // The fenced `### Fake` is inert: exactly ONE finding is enumerated (Real),
    // so it is not a separate delta finding nor a suspected_untagged entry.
    // (Real's block body legitimately includes the fenced sample text — fence
    // inertness means Fake isn't its OWN finding, not that the text is stripped.)
    const QStringList lines = in.split(QLatin1Char('\n'));
    EXPECT_EQ(FeedbackFile::enumerateFindingBlocks(lines).size(), 1);
    EXPECT_TRUE(r.deltaPresent);
    EXPECT_TRUE(r.delta.contains(QStringLiteral("### Real")));
    EXPECT_TRUE(r.suspectedUntagged.isEmpty());
    // The fenced `**Proposed ID:**`-looking line did not contribute an id.
    EXPECT_TRUE(r.mappedIds.isEmpty());
}

// ANTS-3744 — a fully-condensed file (no finding blocks left) harvests its ids
// from the `## Tracked in ROADMAP …` pointer line, so mapped_id_status still
// answers "did my item ship?" for the files tidied hardest.
TEST(FeedbackV2Delta, CondensedTrackedLineSuppliesMappedIds) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# Ants MCP Feedback \xE2\x80\x94 Test\n"
        "\n"
        "> Format: docs/standards/mcp-feedback-files.md.\n"
        "\n"
        "## Tracked in ROADMAP (detail + status there): ANTS-2054, ANTS-3388\n";
    const FeedbackFile::ParseResult r =
        FeedbackFile::parse(QString::fromUtf8(fix));
    EXPECT_EQ(r.formatVersion, 2);
    EXPECT_FALSE(r.deltaPresent);
    ASSERT_EQ(r.mappedIds.size(), 2);
    EXPECT_EQ(r.mappedIds.at(0), QStringLiteral("ANTS-2054"));
    EXPECT_EQ(r.mappedIds.at(1), QStringLiteral("ANTS-3388"));
}

// ANTS-3744 — the pointer line is a FALLBACK: a file that still carries an
// inline `**Proposed ID:**` keeps the pre-3744 inline-only harvest, so a
// partially-condensed file cannot silently gain ids from a stale pointer line.
TEST(FeedbackV2Delta, InlineIdsWinOverTrackedLine) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# T\n"
        "\n"
        "## Tracked in ROADMAP (detail + status there): ANTS-2054\n"
        "\n"
        "### Still open\n"
        "- **Proposed ID:** ANTS-3388\n"
        "- **What:** something.\n";
    const FeedbackFile::ParseResult r =
        FeedbackFile::parse(QString::fromUtf8(fix));
    ASSERT_EQ(r.mappedIds.size(), 1);
    EXPECT_EQ(r.mappedIds.at(0), QStringLiteral("ANTS-3388"));
}

// INV-7 (ANTS-3598) — the fence opener is space-indent-only (CommonMark): a
// TAB-indented ``` is NOT a fence, so a real finding after it is not swallowed.
// Guards against `fenceRe`'s prior `\s{0,3}` (which admitted a tab) silently
// opening a fence and hiding every following finding to EOF.
TEST(FeedbackV2Delta, FenceOpenerIsSpaceIndentOnly) {
    const char *fix =
        "<!-- ants-mcp-feedback: 2 -->\n"                 // 1
        "# T\n"                                           // 2
        "\n"                                              // 3
        "### Real\n"                                      // 4  un-triaged
        "- **Proposed ID:** _(maintainer to assign)_\n"  // 5
        "- **What:** example:\n"                          // 6
        "\t```text\n"                                     // 7  TAB-indent — NOT a fence
        "### AlsoReal\n"                                  // 8  un-triaged
        "- **Proposed ID:** _(maintainer to assign)_\n"  // 9
        "- **What:** second gap.\n";                      // 10
    const QString in = QString::fromUtf8(fix);
    const QStringList lines = in.split(QLatin1Char('\n'));
    // The tab-indented ``` does not open a fence, so BOTH findings enumerate
    // (the buggy `\s{0,3}` opened a fence here and swallowed AlsoReal → 1).
    EXPECT_EQ(FeedbackFile::enumerateFindingBlocks(lines).size(), 2);
    const FeedbackFile::ParseResult r = FeedbackFile::parse(in);
    ASSERT_TRUE(r.deltaPresent);
    EXPECT_TRUE(r.delta.contains(QStringLiteral("### Real")));
    EXPECT_TRUE(r.delta.contains(QStringLiteral("### AlsoReal")));
}

// INV-12 — migrate_v2 → parse round-trip agree on the version.
TEST(FeedbackV2Delta, MigrateThenParseRoundTrip) {
    const char *v1fix =
        "<!-- ants-mcp-feedback: 1 -->\n"
        "# T\n"
        "\n"
        "### Issue #1 \xE2\x80\x94 x\n"
        "- **What:** y.\n";
    const FeedbackFile::MigrateResult mr =
        FeedbackFile::migrateV2(QString::fromUtf8(v1fix));
    const FeedbackFile::ParseResult pr = FeedbackFile::parse(mr.newContent);
    EXPECT_EQ(pr.formatVersion, 2);
    // The stamped placeholder finding is un-triaged.
    ASSERT_TRUE(pr.deltaPresent);
    EXPECT_TRUE(pr.delta.contains(QStringLiteral("_(maintainer to assign)_")));
}

// INV-10 — parse() is pure: the input string is unchanged.
TEST(FeedbackV2Delta, ReadPurity) {
    const QString in = v2();
    const QString copy = in;
    (void)FeedbackFile::parse(in);
    EXPECT_EQ(in, copy);
}

// ---- live wrapper drive ---------------------------------------------------

// INV-8 — feedback_query emits format_version + suspected_untagged[].
TEST(FeedbackV2Delta, LiveEnvelope) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v2()));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("format_version").toInt(), 2);
    const QJsonArray sus = env.value("suspected_untagged").toArray();
    ASSERT_EQ(sus.size(), 1);
    EXPECT_TRUE(sus.at(0).toObject().value("heading").toString()
                    .contains(QStringLiteral("Issue #3")));
    // The delta is Issue #2's block.
    EXPECT_TRUE(env.value("delta_present").toBool());
    EXPECT_EQ(env.value("delta_line_count").toInt(), 3);
    EXPECT_EQ(env.value("delta_start_line").toInt(), 16);
    // mapped_ids: inline only.
    const QJsonArray mids = env.value("mapped_ids").toArray();
    ASSERT_EQ(mids.size(), 1);
    EXPECT_EQ(mids.at(0).toString(), QStringLiteral("ANTS-1525"));
}

// INV-8 — a v1 file: format_version 1, suspected_untagged empty.
TEST(FeedbackV2Delta, LiveV1Envelope) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, v1()));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("format_version").toInt(), 1);
    EXPECT_TRUE(env.value("suspected_untagged").toArray().isEmpty());
}

// INV-9 — feedback_pending count follows the v2 rule via parse(). The
// feedback_pending block reads pr.deltaPresent / pr.deltaLineCount, so the
// version-correct parse() IS the pending count (no code change on that path).
TEST(FeedbackV2Delta, FeedbackPendingCountRule) {
    // §2.5 fixture: exactly one un-triaged finding, 3 lines → the pending count.
    const FeedbackFile::ParseResult r = FeedbackFile::parse(v2());
    EXPECT_TRUE(r.deltaPresent);
    EXPECT_EQ(r.deltaLineCount, 3);   // NOT the v1 after-table region count
}

// Schema — feedback_query description enumerates the new outputs.
TEST(FeedbackV2Delta, SchemaDeclaresOutputs) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("format_version"), std::string::npos);
    EXPECT_NE(ci.find("suspected_untagged"), std::string::npos);
}

// Dispatch — cmdFeedbackQuery emits the new fields.
TEST(FeedbackV2Delta, DispatchWired) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("out[\"format_version\"]"), std::string::npos);
    EXPECT_NE(rc.find("out[\"suspected_untagged\"]"), std::string::npos);
}
