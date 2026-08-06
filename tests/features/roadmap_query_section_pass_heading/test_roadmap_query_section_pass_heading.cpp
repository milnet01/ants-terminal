// Feature-conformance test for ANTS-2225 — roadmap_query section= surfaces a
// nested `#### Pass N.M` bullet even when the single-section slice falls below
// the pass-headings 2+2 detection threshold. INV-1..INV-3 drive the pure
// RoadmapDialog::parseBullets (the mechanism the section= fallback relies on);
// INV-4 source-greps the handler wiring (the section= branch needs a live
// MainWindow). See spec.md + ROADMAP ANTS-2225 (RetroDB feedback Pass 49.1).

#include "../../_support/expect.h"
#include "roadmapdialog.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// A pass-headings roadmap (no ants-v1 emoji bullets) with TWO `#### Pass N.M`
// headings + TWO `- **Status**:` markers — so the whole-file parse engages the
// adapter — split across two `###` sections. The target section ("Indexer
// subsystem") holds exactly ONE pass heading wrapped in prose follow-ups, so
// its slice alone is below the 2+2 threshold.
const char *kSectionA =
    "### Indexer subsystem\n"
    "Mostly prose describing the indexer. No `[PROJ-NNNN]` bullets here,\n"
    "just narrative around a single pass.\n"
    "\n"
    "#### Pass 49.1 (HIGH, M) Wire the section indexer\n"
    "- **Status**: planned\n"
    "- Detail: build the per-section index.\n"
    "\n"
    "A trailing prose paragraph after the pass — which is exactly why the\n"
    "slice classifies as a prose-shaped section.\n"
    "\n";

const char *kSectionB =
    "### Cleanup subsystem\n"
    "#### Pass 50.2 (LOW, S) Remove dead code\n"
    "- **Status**: in-progress\n"
    "- Detail: drop the legacy path.\n";

QString wholeDoc() {
    return QString::fromLatin1(
        "# Roadmap\n\n## MCP Work\n\n") +
        QString::fromLatin1(kSectionA) + QString::fromLatin1(kSectionB);
}

QString sectionASlice() { return QString::fromLatin1(kSectionA); }

const RoadmapDialog::BulletRecord *
findById(const QVector<RoadmapDialog::BulletRecord> &v, const char *id) {
    for (const auto &b : v)
        if (b.id == QLatin1String(id)) return &b;
    return nullptr;
}

}  // namespace

// INV-1 — the whole-file parse engages the pass-headings adapter and surfaces
// PASS-49-1, tagged with its section's global slug.
TEST(RoadmapQuerySectionPassHeading, WholeFileSurfacesPassBullet) {
    const auto bullets = RoadmapDialog::parseBullets(wholeDoc());
    const auto *b = findById(bullets, "PASS-49-1");
    ASSERT_NE(b, nullptr) << "whole-file parse must synthesise PASS-49-1";
    EXPECT_EQ(b->sectionSlug.toStdString(), "indexer-subsystem");
}

// INV-2 — the section slice ALONE has one pass heading + one status marker, so
// the 2+2 detection threshold is unmet and parseBullets returns zero. This is
// the reproduced gap.
TEST(RoadmapQuerySectionPassHeading, SliceLocalDetectionFails) {
    const auto bullets = RoadmapDialog::parseBullets(sectionASlice());
    EXPECT_TRUE(bullets.empty())
        << "a single-section slice must fall below the pass-headings 2+2 "
           "threshold (reproduces the section= count:0 bug)";
}

// INV-3 — filtering the whole-file parse by the section's slug recovers exactly
// the section's pass bullet: the mechanism the section= fallback uses.
TEST(RoadmapQuerySectionPassHeading, WholeFileFilterRecovers) {
    const auto whole = RoadmapDialog::parseBullets(wholeDoc());
    QVector<RoadmapDialog::BulletRecord> filtered;
    for (const auto &b : whole)
        if (b.sectionSlug == QLatin1String("indexer-subsystem"))
            filtered.append(b);
    ASSERT_EQ(filtered.size(), 1);
    EXPECT_EQ(filtered.front().id.toStdString(), "PASS-49-1");
}

// INV-4 — the section= cache-miss branch re-parses the whole markdown and
// filters by sec->slug on an empty slice, before the shape classification.
TEST(RoadmapQuerySectionPassHeading, FallbackWired) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(contains(rc, "ANTS-2225"),
           "INV-4a", "remotecontrol.cpp missing the ANTS-2225 marker");
    expect(contains(rc, "RoadmapDialog::parseBullets(markdown)"),
           "INV-4b",
           "section= fallback must re-parse the whole markdown");
    expect(contains(rc, "b.sectionSlug == sec->slug"),
           "INV-4c",
           "section= fallback must filter whole-file bullets by sec->slug");
    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-2225 wiring invariant(s) failed";
}
