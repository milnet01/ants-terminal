// Feature-conformance test for ANTS-4608 — a C++ scope operator is not a
// trailer declaration.
// Contract and invariant table:
// tests/features/roadmap_trailer_scope_operator/spec.md.
//
// Drives RoadmapParse::trailerValuesIn() directly, where the patterns live.

#include <gtest/gtest.h>

#include "roadmapparse.h"

#include <QString>
#include <QStringList>

// INV-1 — every key, in the shape that actually occurs: prose naming a
// scoped symbol. Before the lookahead, `Source:` matched inside
// `RoadmapSource::bulletsFor()` and captured the remainder of the line.
TEST(RoadmapTrailerScopeOperator, Inv1ScopeOperatorDeclaresNothing) {
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
            "Reads the bullets via RoadmapSource::bulletsFor() and caches."));
        EXPECT_TRUE(tv.source.value.isEmpty())
            << "got: " << tv.source.value.toStdString();
    }
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
            "Dispatch goes through WorkKind::classify() before the write."));
        EXPECT_TRUE(tv.kind.value.isEmpty())
            << "got: " << tv.kind.value.toStdString();
    }
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
            "The owner set is Lanes::all(), which the partition reads."));
        EXPECT_TRUE(tv.lanes.value.isEmpty())
            << "got: " << tv.lanes.value.toStdString();
        EXPECT_TRUE(tv.lanesList.isEmpty());
    }
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
            "Layman::render() is the summary path.\n"));
        EXPECT_TRUE(tv.layman.value.isEmpty())
            << "got: " << tv.layman.value.toStdString();
    }
    {
        const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
            "Evidence::paths() returns the attachment list.\n"));
        EXPECT_TRUE(tv.evidence.value.isEmpty())
            << "got: " << tv.evidence.value.toStdString();
        EXPECT_TRUE(tv.evidenceList.isEmpty());
    }
}

// INV-2 — the guard removes a false match without removing a true one that
// shares the body. This is the case the defect made unwritable: a note that
// names a symbol AND carries real trailers.
TEST(RoadmapTrailerScopeOperator, Inv2RealDeclarationStillParses) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "Reads them via RoadmapSource::bulletsFor().\n"
        "Kind: fix.\n"
        "Source: in-session-2026-08-25.\n"
        "Lanes: mcp, roadmap-store.\n"));
    EXPECT_EQ(tv.kind.value, QStringLiteral("fix"));
    EXPECT_EQ(tv.source.value, QStringLiteral("in-session-2026-08-25"));
    EXPECT_EQ(tv.lanesList,
              (QStringList{QStringLiteral("mcp"),
                           QStringLiteral("roadmap-store")}));
}

// INV-3 — the stop-marker patterns carry the lookahead too. A scoped symbol
// inside a value must not end that value early; the symptom would be a
// silently short column rather than a refusal, which is harder to notice.
TEST(RoadmapTrailerScopeOperator, Inv3ScopeOperatorDoesNotTruncate) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "Source: measured against Lanes::all() and the live store.\n"));
    EXPECT_EQ(tv.source.value,
              QStringLiteral("measured against Lanes::all() and the live store"));
}

// INV-4 — the bold form cannot be rejected by this lookahead, because what
// follows its colon is an asterisk. Pinned so the guard stays a narrowing of
// what the standard already names rather than a new restriction on authors.
TEST(RoadmapTrailerScopeOperator, Inv4BoldLabelStillParses) {
    const auto tv = RoadmapParse::trailerValuesIn(QStringLiteral(
        "**Kind:** fix.\n"
        "**Layman:** A short summary for a non-technical reader.\n"));
    EXPECT_EQ(tv.kind.value, QStringLiteral("fix"));
    EXPECT_EQ(tv.layman.value,
              QStringLiteral("A short summary for a non-technical reader"));
}
