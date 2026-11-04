// ANTS-3504 — the feedback ship-date extractor and feedback_query's
// shipped_date. This bundle held feedback_log op:"compact_resolved"
// (ANTS-3443) until ANTS-4513 retired the op; its op cases went with it
// (ANTS-4513 spec § 7) and these, which feedback_query still needs, stayed.

#include "feedbackfile.h"
#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>
#include "../../_support/srcgrep.h"  // ANTS-3833 — slurpRemoteControl

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
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

// ===== ANTS-3504 — ship-date stamp ====================================

// §2.1 extractor: both paren + no-paren forms, Progress-then-Resolved,
// last-wins, and the graceful misses (mid-line, "Resolved as of", prose,
// absent) all yield the right date or "".
TEST(FeedbackShipDate, ExtractorBothFormsAndMisses) {
    using FeedbackFile::shipDateFromRoadmapBody;
    EXPECT_EQ(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved (2026-07-09): did the thing.")),
        QStringLiteral("2026-07-09"));                       // parenthesised
    EXPECT_EQ(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved 2026-05-29: replaced the map.")),
        QStringLiteral("2026-05-29"));                       // un-parenthesised
    EXPECT_EQ(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved 2026-05-26 (in-session bundle).")),
        QStringLiteral("2026-05-26"));            // no-paren date + note paren
    EXPECT_EQ(shipDateFromRoadmapBody(
        QStringLiteral("  Progress (2026-07-01): partial.\n"
                       "  Resolved (2026-07-09): done.")),
        QStringLiteral("2026-07-09"));         // Progress before final Resolved
    EXPECT_EQ(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved 2026-05-01: first.\n"
                       "  Resolved 2026-06-02: reopened then closed.")),
        QStringLiteral("2026-06-02"));                       // last Resolved wins
    // graceful misses → ""
    EXPECT_TRUE(shipDateFromRoadmapBody(
        QStringLiteral("  no longer shows up.** Resolved 2026-05-08 by X."))
        .isEmpty());                                         // mid-line
    EXPECT_TRUE(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved as of 2026-04-28.")).isEmpty()); // word between
    EXPECT_TRUE(shipDateFromRoadmapBody(
        QStringLiteral("  Resolved the deadlock in the parser.")).isEmpty()); // prose
    EXPECT_TRUE(shipDateFromRoadmapBody(
        QStringLiteral("  What: a description, no resolution line."))
        .isEmpty());                                         // no Resolved line
}

// INV-4 — feedback_query.mapped_id_status carries shipped_date on ✅ ids with
// a Resolved date, and omits it for a non-✅ id.
TEST(FeedbackShipDate, LiveQueryEmitsShippedDate) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = dir.path() + "/TEST_Ants_MCP_Feedback.md";
    ASSERT_TRUE(writeStr(p, fx()));
    const char *rm =
        "# Roadmap\n\n"
        "- \xE2\x9C\x85 [ANTS-1525] **fixed.**\n"
        "  Resolved (2026-07-12): done.\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-1600] **open.**\n"
        "  Progress (2026-07-01): wip.\n";
    ASSERT_TRUE(writeStr(dir.path() + "/ROADMAP.md", QString::fromUtf8(rm)));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req["path"] = p;
    req["caller_cwd"] = dir.path();
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value("ok").toBool());
    const QString check = QString::fromUtf8("\xE2\x9C\x85");
    bool saw1525 = false, saw1600 = false;
    for (const auto &v : env.value("mapped_id_status").toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value("id").toString() == QStringLiteral("ANTS-1525")) {
            saw1525 = true;
            EXPECT_EQ(o.value("status").toString(), check);
            EXPECT_EQ(o.value("shipped_date").toString(),
                      QStringLiteral("2026-07-12"));
        }
        if (o.value("id").toString() == QStringLiteral("ANTS-1600")) {
            saw1600 = true;
            EXPECT_FALSE(o.contains(QStringLiteral("shipped_date")))
                << "a non-✅ id must not carry shipped_date";
        }
    }
    EXPECT_TRUE(saw1525) << "ANTS-1525 must appear in mapped_id_status";
    EXPECT_TRUE(saw1600) << "ANTS-1600 must appear in mapped_id_status";
}

// Wiring — both surfaces populate the ship-date via the shared extractor, and
// the feedback_query description documents shipped_date.
TEST(FeedbackShipDate, WiringSharedExtractor) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("FeedbackFile::shipDateFromRoadmapBody"), std::string::npos);
    EXPECT_NE(rc.find("shipped_date"), std::string::npos);
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_NE(ci.find("shipped_date"), std::string::npos)
        << "feedback_query description must document shipped_date";
}
