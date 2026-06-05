// ANTS-2035 — feature-conformance test for pass-heading sub-pass ID
// synthesis. Behavioural test against RoadmapDialog::parseBullets's
// pass-headings dispatch (no real ROADMAP.md needed). Reproduces the
// RetroDB false-duplicate set (parent 41.5 vs sub-pass 41.5.B both
// synthesising PASS-41-5) and locks in the distinct-ID fix.

#include "roadmapindex.h"
#include "roadmapdialog.h"

#include <gtest/gtest.h>

#include <QHash>
#include <QString>
#include <QStringLiteral>

namespace {

// A pass-headings doc with a parent + two .LETTER sub-passes. The
// format sniffer requires ≥2 `#### Pass` headings + ≥2 `- **Status**:`
// markers and no ants-v1 emoji bullets, which this satisfies.
QString subPassDoc() {
    return QStringLiteral(
        "## Active\n"
        "\n"
        "#### Pass 41.5 (CRITICAL, S) Parent work item\n"
        "- **Status**: in-progress\n"
        "- **Finding**: the parent.\n"
        "\n"
        "#### Pass 41.5.A (HIGH, M) First sub-pass\n"
        "- **Status**: todo\n"
        "- **Finding**: sub-pass A.\n"
        "\n"
        "#### Pass 41.5.B (HIGH, M) Second sub-pass\n"
        "- **Status**: todo\n"
        "- **Finding**: sub-pass B.\n");
}

// Mirror of rcComputeDuplicateIds' canonical-keyed dedup (which lives
// in remotecontrol.cpp's anonymous namespace and is not linkable
// here). Returns the count of canonical ids seen on >1 bullet.
int canonicalDuplicateCount(const QVector<RoadmapDialog::BulletRecord> &b) {
    QHash<QString, int> seen;
    for (const auto &rec : b) {
        if (!RoadmapIndex::isCanonicalId(rec.id)) continue;
        ++seen[rec.id];
    }
    int dupes = 0;
    for (auto it = seen.cbegin(); it != seen.cend(); ++it) {
        if (it.value() > 1) ++dupes;
    }
    return dupes;
}

}  // namespace

// INV-1 — parent and each .LETTER sub-pass get distinct ids.
TEST(roadmap_parser_subpass_id, Inv1DistinctIds) {
    const auto bullets = RoadmapDialog::parseBullets(subPassDoc());
    ASSERT_EQ(bullets.size(), 3)
        << "precondition: doc must parse as 3 pass-heading bullets";
    EXPECT_EQ(bullets[0].id, QStringLiteral("PASS-41-5"))
        << "INV-1: parent keeps PASS-<major>-<minor>";
    EXPECT_EQ(bullets[1].id, QStringLiteral("PASS-41-5-A"))
        << "INV-1: sub-pass .A carries the suffix in its id";
    EXPECT_EQ(bullets[2].id, QStringLiteral("PASS-41-5-B"))
        << "INV-1: sub-pass .B carries the suffix in its id";
    EXPECT_NE(bullets[0].id, bullets[2].id)
        << "INV-1: parent and sub-pass must not collide (the bug)";
}

// INV-2 — no false duplicate from a parent + its sub-passes.
TEST(roadmap_parser_subpass_id, Inv2NoFalseDuplicate) {
    const auto bullets = RoadmapDialog::parseBullets(subPassDoc());
    EXPECT_EQ(canonicalDuplicateCount(bullets), 0)
        << "INV-2: parent + .LETTER sub-passes must not be flagged as "
           "duplicate ids (RetroDB false PASS-41-5 set)";
}

// INV-3 — a plain heading with no sub-pass is byte-for-byte unchanged.
TEST(roadmap_parser_subpass_id, Inv3ParentOnlyUnchanged) {
    const QString md = QStringLiteral(
        "## Active\n"
        "\n"
        "#### Pass 7.2 (CRITICAL, S) Fix the thing\n"
        "- **Status**: done\n"
        "\n"
        "#### Pass 7.3 (LOW, S) Another thing\n"
        "- **Status**: todo\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].id, QStringLiteral("PASS-7-2"));
    EXPECT_EQ(bullets[1].id, QStringLiteral("PASS-7-3"));
}

// INV-4 — a numeric third level is NOT treated as a sub-pass (the
// suffix is letter-led); it stays in the headline tail as before.
TEST(roadmap_parser_subpass_id, Inv4NumericThirdLevelNotSubPass) {
    const QString md = QStringLiteral(
        "## Active\n"
        "\n"
        "#### Pass 3.1.2 (LOW, S) Numeric third level\n"
        "- **Status**: todo\n"
        "\n"
        "#### Pass 3.2 (LOW, S) Sibling\n"
        "- **Status**: todo\n");
    const auto bullets = RoadmapDialog::parseBullets(md);
    ASSERT_EQ(bullets.size(), 2);
    EXPECT_EQ(bullets[0].id, QStringLiteral("PASS-3-1"))
        << "INV-4: numeric .2 falls into the tail, not the id "
           "(documented .LETTER scope only)";
}
