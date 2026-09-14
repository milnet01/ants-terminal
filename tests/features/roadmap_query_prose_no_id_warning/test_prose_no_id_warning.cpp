// ANTS-3583 — feature-conformance test for roadmap_query prose-roadmap
// warning parity across status filters. Source-scrape harness (matching the
// sibling roadmap_query_narrator_filter / mcp_roadmap_status_filter tests);
// no GUI, no Roadmap fixture.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#include "remotecontrol.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — on the empty-result path the handler scans the whole cached
// bullet set (file-level, filter-independent) for any id-bearing bullet.
TEST(roadmap_query_prose_no_id_warning, Inv1FileLevelIdBearingScan) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-3583"),
           "INV-1: ANTS-3583 anchor present in remotecontrol.cpp");
    expect(contains(cpp, "fileHasIdBearingBullet"),
           "INV-1: file-level id-bearing scan flag present");
    // The scan iterates the cached bullet set through the same drop helper
    // the emission loop uses, so 'id-bearing' means the same thing here.
    expect(contains(cpp, "std::as_const(m_roadmapCacheBullets)"),
           "INV-1: scan iterates the whole cached bullet set");
    expect(contains(cpp, "fileHasIdBearingBullet = true; break;"),
           "INV-1: early-exit on the first id-bearing bullet keeps it O(1)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — zero id-bearing bullets => parseable_bullets:0 + a "format not
// recognised" warning, on every status filter.
TEST(roadmap_query_prose_no_id_warning, Inv2ParseableBulletsSignalAndWarning) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "out[\"parseable_bullets\"] = 0"),
           "INV-2: machine-detectable parseable_bullets:0 emitted");
    expect(contains(cpp, "no [PROJ-NNNN]-tagged bullets that "),
           "INV-2: warning names the unrecognised-format cause");
    expect(contains(cpp, "NOT \\\"no outstanding "),
           "INV-2: warning explicitly disclaims the 'no work left' reading");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — filter-independence: the new branch is gated on the file-level
// scan + the two opt-in flags, NOT on the post-status preIdPruneCountFull
// (the value the status filter was able to zero). It must precede the
// ANTS-1538 preIdPruneCountFull branch.
TEST(roadmap_query_prose_no_id_warning, Inv3GatedOnFileScanNotPostStatusCount) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "if (!fileHasIdBearingBullet &&"),
           "INV-3: branch gated on the file-level scan (not the post-status "
           "count)");
    // Ordering: the ANTS-3583 parseable_bullets branch must appear before the
    // ANTS-1538 preIdPruneCountFull branch so it supersedes it for the
    // prose-roadmap case on status:'all' too.
    const std::string::size_type parseablePos =
        cpp.find("out[\"parseable_bullets\"] = 0");
    const std::string::size_type preIdPrunePos =
        cpp.find("} else if (preIdPruneCountFull > 0 &&");
    expect(parseablePos != std::string::npos &&
           preIdPrunePos != std::string::npos &&
           parseablePos < preIdPrunePos,
           "INV-3: parseable_bullets branch precedes the preIdPruneCountFull "
           "branch");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-4971 — a kind filter that empties a set of id-bearing bullets is named
// as the cause. The ID-filter warning, which called a well-formed roadmap
// malformed, no longer fires. Live, through the real verb, in both the
// full-file and the section arm.
TEST(roadmap_query_prose_no_id_warning, Ants4971KindFilterIsNamedNotTheIdFilter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    {
        QFile f(tmp.path() + QStringLiteral("/ROADMAP.md"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("# Roadmap\n\n## Work\n\n"
                "- \xF0\x9F\x93\x8B [ANTS-7001] **First fix.**\n"
                "  Kind: fix.\n"
                "  Source: test.\n"
                "- \xF0\x9F\x93\x8B [ANTS-7002] **Second fix.**\n"
                "  Kind: fix.\n"
                "  Source: test.\n");
    }
    for (const bool withSection : {false, true}) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = tmp.path();
        req[QStringLiteral("status")]     = QStringLiteral("planned");
        req[QStringLiteral("kind")]       = QStringLiteral("release");
        if (withSection) req[QStringLiteral("section")] = QStringLiteral("work");
        RemoteControl rc(nullptr);
        const QJsonObject o = rc.cmdRoadmapQuery(req).object();
        const char *arm = withSection ? "section" : "full-file";
        ASSERT_TRUE(o.value(QStringLiteral("ok")).toBool())
            << arm << ": " << QJsonDocument(o).toJson().constData();
        EXPECT_EQ(o.value(QStringLiteral("count")).toInt(), 0) << arm;
        const QString w = o.value(QStringLiteral("warning")).toString();
        EXPECT_FALSE(w.contains(QStringLiteral("default ID-filter dropped")))
            << arm << " arm blamed the ID filter: " << w.toStdString();
        EXPECT_TRUE(w.contains(QStringLiteral("kind/source filter")))
            << arm << " arm did not name the filter that emptied the set: "
            << w.toStdString();
    }
}
