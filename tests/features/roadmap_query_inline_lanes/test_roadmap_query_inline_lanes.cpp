// ANTS-2058 — feature-conformance test: parseBullets must populate `lanes`
// when the metadata is written inline as one prose sentence
// (`Kind: X. Lanes: Y.`). Pre-fix rxLanes carried a `^` line-start anchor,
// so a mid-line `Lanes:` never matched and lanes came back empty while
// kind (same line) parsed fine. Drives the pure static parseBullets.

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QVector>

ANTS_TEST_SCOPE();

namespace {

// 📋 = U+1F4CB (F0 9F 93 8B). Two bullets carry inline `Kind: . Lanes: .`
// metadata on a single continuation line.
const char *kInlineSeed =
    "# Roadmap\n\n"
    "## Work\n\n"
    "- \xF0\x9F\x93\x8B [ANTS-1000] **Inline-metadata bullet.**\n"
    "  Kind: refactor. Lanes: backend tests. Source: seed-2026-06-10.\n"
    "- \xF0\x9F\x93\x8B [ANTS-1001] **Multi-lane inline bullet.**\n"
    "  Kind: fix. Lanes: frontend, ui, theming. Source: seed.\n";

}  // namespace

// INV-1 — inline `Lanes:` after `Kind:` on the same line parses; kind too.
TEST(roadmap_query_inline_lanes, Inv1InlineLanesParsed) {
    const auto bullets =
        RoadmapDialog::parseBullets(QString::fromUtf8(kInlineSeed));
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets.at(0).kind, QStringLiteral("refactor"))
        << "kind still parses from the shared sentence";
    ASSERT_EQ(bullets.at(0).lanes.size(), 1)
        << "inline Lanes: must populate (was [] pre-fix)";
    EXPECT_EQ(bullets.at(0).lanes.at(0), QStringLiteral("backend tests"));
}

// INV-2 — comma-separated inline lanes split into N trimmed entries.
TEST(roadmap_query_inline_lanes, Inv2MultiLaneSplit) {
    const auto bullets =
        RoadmapDialog::parseBullets(QString::fromUtf8(kInlineSeed));
    ASSERT_EQ(bullets.size(), 2);
    const QStringList got = bullets.at(1).lanes;
    ASSERT_EQ(got.size(), 3);
    EXPECT_EQ(got.at(0), QStringLiteral("frontend"));
    EXPECT_EQ(got.at(1), QStringLiteral("ui"));
    EXPECT_EQ(got.at(2), QStringLiteral("theming"));
}

// INV-3 — line-leading `Lanes:` on its own continuation line still works.
TEST(roadmap_query_inline_lanes, Inv3LineLeadingStillWorks) {
    const char *seed =
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-1002] **Own-line metadata.**\n"
        "  Kind: doc.\n"
        "  Lanes: docs, standards.\n";
    const auto bullets =
        RoadmapDialog::parseBullets(QString::fromUtf8(seed));
    ASSERT_EQ(bullets.size(), 1);
    ASSERT_EQ(bullets.at(0).lanes.size(), 2);
    EXPECT_EQ(bullets.at(0).lanes.at(0), QStringLiteral("docs"));
    EXPECT_EQ(bullets.at(0).lanes.at(1), QStringLiteral("standards"));
}
