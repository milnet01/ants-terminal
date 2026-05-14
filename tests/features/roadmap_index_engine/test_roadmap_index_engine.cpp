// Feature-conformance test for ANTS-1287 RoadmapIndex.
// See tests/features/roadmap_index_engine/spec.md.

#include <gtest/gtest.h>

#include "roadmapindex.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace {

QString sampleDoc() {
    QStringList lines;
    lines << "# Top"                    // 0  H1 (not indexed)
          << ""                          // 1
          << "## Performance"            // 2  H2 — slug "performance"
          << "- bullet a"                // 3
          << ""                          // 4
          << "### Subsection"            // 5  H3 — slug "subsection"
          << "- bullet b"                // 6
          << ""                          // 7
          << "#### Deeper"               // 8  H4 (not indexed)
          << "- bullet c"                // 9
          << ""                          // 10
          << "## Performance"            // 11 H2 — slug "performance-2"
          << "- bullet d"                // 12
          << "### Performance"           // 13 H3 — slug "performance-3"
          << "- bullet e"                // 14
          << ""                          // 15
          << "## Trailing";              // 16 H2 — slug "trailing"
    return lines.join('\n');
}

}  // namespace

// ENG-1
TEST(RoadmapIndexEngine, IndexesH2AndH3Only) {
    const auto idx = RoadmapIndex::buildIndex(sampleDoc());
    ASSERT_EQ(idx.size(), 5);
    EXPECT_EQ(idx[0].level, 2);  // ## Performance
    EXPECT_EQ(idx[1].level, 3);  // ### Subsection
    EXPECT_EQ(idx[2].level, 2);  // ## Performance (dup)
    EXPECT_EQ(idx[3].level, 3);  // ### Performance (dup)
    EXPECT_EQ(idx[4].level, 2);  // ## Trailing
}

// ENG-2
TEST(RoadmapIndexEngine, LineStartAndExclusiveLineEnd) {
    const auto idx = RoadmapIndex::buildIndex(sampleDoc());
    ASSERT_EQ(idx.size(), 5);
    // ## Performance: line 2 → ends at next H2 (line 11)
    EXPECT_EQ(idx[0].lineStart, 2);
    EXPECT_EQ(idx[0].lineEnd, 11);
    // ### Subsection: line 5 → ends at next H3-or-H2 (line 11)
    EXPECT_EQ(idx[1].lineStart, 5);
    EXPECT_EQ(idx[1].lineEnd, 11);
    // ## Performance (dup): line 11 → ends at next H2 (line 16)
    EXPECT_EQ(idx[2].lineStart, 11);
    EXPECT_EQ(idx[2].lineEnd, 16);
    // ### Performance (dup): line 13 → ends at next H3-or-H2 (line 16)
    EXPECT_EQ(idx[3].lineStart, 13);
    EXPECT_EQ(idx[3].lineEnd, 16);
    // ## Trailing: line 16 → ends at total_lines (17)
    EXPECT_EQ(idx[4].lineStart, 16);
    EXPECT_EQ(idx[4].lineEnd, 17);
}

// ENG-3
TEST(RoadmapIndexEngine, DuplicateHeadingsGetSuffixedSlugs) {
    const auto idx = RoadmapIndex::buildIndex(sampleDoc());
    ASSERT_EQ(idx.size(), 5);
    EXPECT_EQ(idx[0].slug, QString("performance"));
    EXPECT_EQ(idx[1].slug, QString("subsection"));
    EXPECT_EQ(idx[2].slug, QString("performance-2"));
    EXPECT_EQ(idx[3].slug, QString("performance-3"));
    EXPECT_EQ(idx[4].slug, QString("trailing"));
}

// ENG-4
TEST(RoadmapIndexEngine, FindBySlugReturnsHitOrNullptr) {
    const auto idx = RoadmapIndex::buildIndex(sampleDoc());
    const auto *hit = RoadmapIndex::findBySlug(idx, "performance-2");
    ASSERT_NE(hit, nullptr);
    EXPECT_EQ(hit->slug, QString("performance-2"));
    EXPECT_EQ(hit->level, 2);
    EXPECT_EQ(RoadmapIndex::findBySlug(idx, "nope"), nullptr);
    EXPECT_EQ(RoadmapIndex::findBySlug(idx, ""), nullptr);
}

// ENG-5
TEST(RoadmapIndexEngine, SliceSectionReturnsExclusiveLineRange) {
    const QString doc = sampleDoc();
    const auto idx = RoadmapIndex::buildIndex(doc);
    const auto *sec = RoadmapIndex::findBySlug(idx, "performance");
    ASSERT_NE(sec, nullptr);
    const QString slice = RoadmapIndex::sliceSection(doc, *sec);
    // First line is the heading itself.
    ASSERT_TRUE(slice.startsWith("## Performance"));
    // Last line is the line BEFORE the next heading — "## Performance"
    // (the dup at line 11) must NOT appear.
    EXPECT_EQ(slice.count("## Performance"), 1);
    // Subsection content IS inside the slice.
    EXPECT_TRUE(slice.contains("### Subsection"));
    EXPECT_TRUE(slice.contains("bullet b"));
    EXPECT_TRUE(slice.contains("#### Deeper"));
}

// ENG-6
TEST(RoadmapIndexEngine, SlugifyHeadingMatchesOldStaticRules) {
    EXPECT_EQ(RoadmapIndex::slugifyHeading("Performance"),
              QString("performance"));
    EXPECT_EQ(RoadmapIndex::slugifyHeading("0.8.0 — Multiplexing"),
              QString("0-8-0-multiplexing"));
    EXPECT_EQ(RoadmapIndex::slugifyHeading("Trailing dashes--"),
              QString("trailing-dashes"));
    EXPECT_EQ(RoadmapIndex::slugifyHeading("   "),
              QString());
    // Leading non-alnum should not produce a leading dash.
    EXPECT_FALSE(RoadmapIndex::slugifyHeading("— Theme").startsWith('-'));
}

// ENG-7
TEST(RoadmapIndexEngine, NestedSectionScoping) {
    // ## parent
    //   ### childA
    //   ### childB
    // ## sibling
    QStringList lines;
    lines << "## parent" << "x"
          << "### childA" << "a"
          << "### childB" << "b"
          << "## sibling" << "s";
    const QString doc = lines.join('\n');
    const auto idx = RoadmapIndex::buildIndex(doc);
    ASSERT_EQ(idx.size(), 4);
    // parent (line 0) ends at sibling (line 6).
    EXPECT_EQ(idx[0].slug, QString("parent"));
    EXPECT_EQ(idx[0].lineEnd, 6);
    // childA (line 2) ends at childB (line 4) — same level.
    EXPECT_EQ(idx[1].slug, QString("childa"));
    EXPECT_EQ(idx[1].lineEnd, 4);
    // childB (line 4) ends at sibling (line 6) — sibling has lower level.
    EXPECT_EQ(idx[2].slug, QString("childb"));
    EXPECT_EQ(idx[2].lineEnd, 6);
}

// Uniqueness walk: empty heading slug must NOT consume a counter,
// otherwise an unrelated subsequent "## foo" could end up as "foo-2".
TEST(RoadmapIndexEngine, EmptyHeadingDoesNotConsumeCounter) {
    QSet<QString> seen;
    EXPECT_EQ(RoadmapIndex::uniqueSlug(seen, ""), QString());
    EXPECT_EQ(RoadmapIndex::uniqueSlug(seen, "foo"), QString("foo"));
    EXPECT_EQ(RoadmapIndex::uniqueSlug(seen, "foo"), QString("foo-2"));
}
